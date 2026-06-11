/*
 * Copyright (C) 2012-2022 Apple Inc. All rights reserved.
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

#include "ConcurrentJSLock.h"
#include "Structure.h"
#include <wtf/Atomics.h>
#include <wtf/OptionSet.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

class CodeBlock;
class LLIntOffsetsExtractor;
class UnlinkedArrayProfile;

// This is a bitfield where each bit represents an type of array access that we have seen.
// There are 19 indexing types that use the lower bits.
// There are 11 typed array types taking the bits 16-20 and 26-31.
typedef unsigned ArrayModes;

// The possible IndexingTypes are limited within (0 - 16, 21, 23, 25).
// This is because CoW types only appear for JSArrays.
static_assert(CopyOnWriteArrayWithInt32 == 21);
static_assert(CopyOnWriteArrayWithDouble == 23);
static_assert(CopyOnWriteArrayWithContiguous == 25);
const ArrayModes CopyOnWriteArrayWithInt32ArrayMode = 1 << CopyOnWriteArrayWithInt32;
const ArrayModes CopyOnWriteArrayWithDoubleArrayMode = 1 << CopyOnWriteArrayWithDouble;
const ArrayModes CopyOnWriteArrayWithContiguousArrayMode = 1 << CopyOnWriteArrayWithContiguous;

const ArrayModes Int8ArrayMode = 1U << 16;
const ArrayModes Int16ArrayMode = 1U << 17;
const ArrayModes Int32ArrayMode = 1U << 18;
const ArrayModes Uint8ArrayMode = 1U << 19;
const ArrayModes Uint8ClampedArrayMode = 1U << 20;
// 21, 23, 25 are used for CoW arrays.
const ArrayModes Float16ArrayMode = 1U << 22;
const ArrayModes Uint16ArrayMode = 1U << 26;
const ArrayModes Uint32ArrayMode = 1U << 27;
const ArrayModes Float32ArrayMode = 1U << 28;
const ArrayModes Float64ArrayMode = 1U << 29;
const ArrayModes BigInt64ArrayMode = 1U << 30;
const ArrayModes BigUint64ArrayMode = 1U << 31;

JS_EXPORT_PRIVATE extern const ArrayModes typedArrayModes[NumberOfTypedArrayTypesExcludingDataView];

constexpr ArrayModes asArrayModesIgnoringTypedArrays(IndexingType indexingMode)
{
    return static_cast<unsigned>(1) << static_cast<unsigned>(indexingMode);
}

#define ALL_TYPED_ARRAY_MODES \
    (Int8ArrayMode            \
    | Int16ArrayMode          \
    | Int32ArrayMode          \
    | Uint8ArrayMode          \
    | Uint8ClampedArrayMode   \
    | Uint16ArrayMode         \
    | Uint32ArrayMode         \
    | Float16ArrayMode        \
    | Float32ArrayMode        \
    | Float64ArrayMode        \
    | BigInt64ArrayMode       \
    | BigUint64ArrayMode      \
    )

#define ALL_NON_ARRAY_ARRAY_MODES                       \
    (asArrayModesIgnoringTypedArrays(NonArray)                             \
    | asArrayModesIgnoringTypedArrays(NonArrayWithInt32)                   \
    | asArrayModesIgnoringTypedArrays(NonArrayWithDouble)                  \
    | asArrayModesIgnoringTypedArrays(NonArrayWithContiguous)              \
    | asArrayModesIgnoringTypedArrays(NonArrayWithArrayStorage)            \
    | asArrayModesIgnoringTypedArrays(NonArrayWithSlowPutArrayStorage)     \
    | ALL_TYPED_ARRAY_MODES)

#define ALL_COPY_ON_WRITE_ARRAY_MODES                   \
    (CopyOnWriteArrayWithInt32ArrayMode                 \
    | CopyOnWriteArrayWithDoubleArrayMode               \
    | CopyOnWriteArrayWithContiguousArrayMode)

#define ALL_WRITABLE_ARRAY_ARRAY_MODES                  \
    (asArrayModesIgnoringTypedArrays(ArrayClass)                           \
    | asArrayModesIgnoringTypedArrays(ArrayWithUndecided)                  \
    | asArrayModesIgnoringTypedArrays(ArrayWithInt32)                      \
    | asArrayModesIgnoringTypedArrays(ArrayWithDouble)                     \
    | asArrayModesIgnoringTypedArrays(ArrayWithContiguous)                 \
    | asArrayModesIgnoringTypedArrays(ArrayWithArrayStorage)               \
    | asArrayModesIgnoringTypedArrays(ArrayWithSlowPutArrayStorage))

#define ALL_ARRAY_ARRAY_MODES                           \
    (ALL_WRITABLE_ARRAY_ARRAY_MODES                     \
    | ALL_COPY_ON_WRITE_ARRAY_MODES)

#define ALL_ARRAY_MODES (ALL_NON_ARRAY_ARRAY_MODES | ALL_ARRAY_ARRAY_MODES)

inline ArrayModes arrayModesFromStructure(Structure* structure)
{
    JSType type = structure->typeInfo().type();
    if (isTypedArrayType(type))
        return typedArrayModes[type - FirstTypedArrayType];
    return asArrayModesIgnoringTypedArrays(structure->indexingMode());
}

void dumpArrayModes(PrintStream&, ArrayModes);
MAKE_PRINT_ADAPTOR(ArrayModesDump, ArrayModes, dumpArrayModes);

inline bool mergeArrayModes(ArrayModes& left, ArrayModes right)
{
    ArrayModes newModes = left | right;
    if (newModes == left)
        return false;
    left = newModes;
    return true;
}

inline bool arrayModesAreClearOrTop(ArrayModes modes)
{
    return !modes || modes == ALL_ARRAY_MODES;
}

// Checks if proven is a subset of expected.
inline bool arrayModesAlreadyChecked(ArrayModes proven, ArrayModes expected)
{
    return (expected | proven) == expected;
}

inline bool arrayModesIncludeIgnoringTypedArrays(ArrayModes arrayModes, IndexingType shape)
{
    ArrayModes modes = asArrayModesIgnoringTypedArrays(NonArray | shape) | asArrayModesIgnoringTypedArrays(ArrayClass | shape);
    if (hasInt32(shape) || hasDouble(shape) || hasContiguous(shape))
        modes |= asArrayModesIgnoringTypedArrays(ArrayClass | shape | CopyOnWrite);
    return !!(arrayModes & modes);
}

inline bool shouldUseSlowPutArrayStorage(ArrayModes arrayModes)
{
    return arrayModesIncludeIgnoringTypedArrays(arrayModes, SlowPutArrayStorageShape);
}

inline bool shouldUseFastArrayStorage(ArrayModes arrayModes)
{
    return arrayModesIncludeIgnoringTypedArrays(arrayModes, ArrayStorageShape);
}

inline bool shouldUseContiguous(ArrayModes arrayModes)
{
    return arrayModesIncludeIgnoringTypedArrays(arrayModes, ContiguousShape);
}

inline bool shouldUseDouble(ArrayModes arrayModes)
{
    ASSERT(Options::allowDoubleShape());
    return arrayModesIncludeIgnoringTypedArrays(arrayModes, DoubleShape);
}

inline bool shouldUseInt32(ArrayModes arrayModes)
{
    return arrayModesIncludeIgnoringTypedArrays(arrayModes, Int32Shape);
}

inline bool hasSeenArray(ArrayModes arrayModes)
{
    return arrayModes & ALL_ARRAY_ARRAY_MODES;
}

inline bool hasSeenNonArray(ArrayModes arrayModes)
{
    return arrayModes & ALL_NON_ARRAY_ARRAY_MODES;
}

inline bool hasSeenWritableArray(ArrayModes arrayModes)
{
    return arrayModes & ALL_WRITABLE_ARRAY_ARRAY_MODES;
}

inline bool hasSeenCopyOnWriteArray(ArrayModes arrayModes)
{
    return arrayModes & ALL_COPY_ON_WRITE_ARRAY_MODES;
}

enum class ArrayProfileFlag : uint32_t {
    MayStoreHole = 1 << 0, // This flag may become overloaded to indicate other special cases that were encountered during array access, as it depends on indexing type. Since we currently have basically just one indexing type (two variants of ArrayStorage), this flag for now just means exactly what its name implies.
    OutOfBounds = 1 << 1,
    MayBeLargeTypedArray = 1 << 2,
    MayInterceptIndexedAccesses = 1 << 3,
    UsesNonOriginalArrayStructures = 1 << 4,
    MayBeResizableOrGrowableSharedTypedArray = 1 << 5,
    DidPerformFirstRunPruning = 1 << 6,
};

class ArrayProfile {
    friend class CodeBlock;
    friend class UnlinkedArrayProfile;
public:
    explicit ArrayProfile() = default;

    void clear()
    {
        // THREADS §5.7.5: relaxed word-atomic stores; clear can race with profiling
        // writers on other mutators, and a lost observation is benign (I12).
        WTF::atomicStore(&m_lastSeenStructureID, StructureID(), std::memory_order_relaxed);
        WTF::atomicStore(&m_speculationFailureStructureID, StructureID(), std::memory_order_relaxed);
        static_assert(sizeof(m_arrayProfileFlags) == sizeof(uint32_t));
        WTF::atomicStore(reinterpret_cast<uint32_t*>(&m_arrayProfileFlags), 0u, std::memory_order_relaxed);
        WTF::atomicStore(&m_observedArrayModes, static_cast<ArrayModes>(0), std::memory_order_relaxed);
    }

    // THREADS §5.7.5 (SPEC-jit Task 12): mode/flag merges are monotone ORs racily performed
    // by mutator slow paths on any of N threads and racily read by compiler threads; a lost
    // bit is benign — profiles select, guards validate (I12). C++ merges use relaxed atomic
    // OR and every C++ read is a relaxed atomic load; JIT'd fast-path merges may stay plain
    // per §5.7.1 (fields keep their plain declared types so JIT offsets/codegen are
    // unchanged). The structure-ID words (m_lastSeenStructureID,
    // m_speculationFailureStructureID) are advisory per §5.7.7 — C++ accesses go through
    // relaxed word-atomics; JIT'd stores stay plain.
    void addArrayProfileFlagsConcurrently(OptionSet<ArrayProfileFlag> flags)
    {
        static_assert(sizeof(m_arrayProfileFlags) == sizeof(uint32_t));
        WTF::atomicExchangeOr(reinterpret_cast<uint32_t*>(&m_arrayProfileFlags), flags.toRaw(), std::memory_order_relaxed);
    }

    OptionSet<ArrayProfileFlag> arrayProfileFlagsConcurrently() const
    {
        static_assert(sizeof(m_arrayProfileFlags) == sizeof(uint32_t));
        return OptionSet<ArrayProfileFlag>::fromRaw(WTF::atomicLoad(reinterpret_cast<uint32_t*>(const_cast<OptionSet<ArrayProfileFlag>*>(&m_arrayProfileFlags)), std::memory_order_relaxed));
    }

    static constexpr uint64_t s_smallTypedArrayMaxLength = std::numeric_limits<int32_t>::max();
    void setMayBeLargeTypedArray() { addArrayProfileFlagsConcurrently(ArrayProfileFlag::MayBeLargeTypedArray); }
    bool mayBeLargeTypedArray(const ConcurrentJSLocker&) const { return arrayProfileFlagsConcurrently().contains(ArrayProfileFlag::MayBeLargeTypedArray); }

    bool mayBeResizableOrGrowableSharedTypedArray(const ConcurrentJSLocker&) const { return arrayProfileFlagsConcurrently().contains(ArrayProfileFlag::MayBeResizableOrGrowableSharedTypedArray); }

    StructureID* addressOfSpeculationFailureStructureID() LIFETIME_BOUND { return &m_speculationFailureStructureID; }
    ArrayModes* addressOfArrayModes() LIFETIME_BOUND { return &m_observedArrayModes; }

    static constexpr ptrdiff_t offsetOfLastSeenStructureID() { return OBJECT_OFFSETOF(ArrayProfile, m_lastSeenStructureID); }
    static constexpr ptrdiff_t offsetOfSpeculationFailureStructureID() { return OBJECT_OFFSETOF(ArrayProfile, m_speculationFailureStructureID); }
    static constexpr ptrdiff_t offsetOfArrayProfileFlags() { return OBJECT_OFFSETOF(ArrayProfile, m_arrayProfileFlags); }
    static constexpr ptrdiff_t offsetOfArrayModes() { return OBJECT_OFFSETOF(ArrayProfile, m_observedArrayModes); }

    void setOutOfBounds() { addArrayProfileFlagsConcurrently(ArrayProfileFlag::OutOfBounds); }
    
    // THREADS §5.7.5/§5.7.7: relaxed word-atomic last-writer-wins stores; the word is
    // advisory to every consumer (I12).
    void observeStructureID(StructureID structureID) { WTF::atomicStore(&m_lastSeenStructureID, structureID, std::memory_order_relaxed); }
    void observeStructure(Structure* structure) { WTF::atomicStore(&m_lastSeenStructureID, structure->id(), std::memory_order_relaxed); }

    void NODELETE computeUpdatedPrediction(CodeBlock*);
    void computeUpdatedPrediction(CodeBlock*, Structure* lastSeenStructure);
    
    void observeArrayMode(ArrayModes mode) { WTF::atomicExchangeOr(&m_observedArrayModes, mode, std::memory_order_relaxed); } // THREADS §5.7.5
    void NODELETE observeIndexedRead(JSCell*, unsigned index);

    ArrayModes observedArrayModes(const ConcurrentJSLocker&) const { return WTF::atomicLoad(const_cast<ArrayModes*>(&m_observedArrayModes), std::memory_order_relaxed); }
    bool mayInterceptIndexedAccesses(const ConcurrentJSLocker&) const { return arrayProfileFlagsConcurrently().contains(ArrayProfileFlag::MayInterceptIndexedAccesses); }

    bool mayStoreToHole(const ConcurrentJSLocker&) const { return arrayProfileFlagsConcurrently().contains(ArrayProfileFlag::MayStoreHole); }
    bool outOfBounds(const ConcurrentJSLocker&) const { return arrayProfileFlagsConcurrently().contains(ArrayProfileFlag::OutOfBounds); }

    bool usesOriginalArrayStructures(const ConcurrentJSLocker&) const { return !arrayProfileFlagsConcurrently().contains(ArrayProfileFlag::UsesNonOriginalArrayStructures); }

    CString briefDescription(CodeBlock*);
    CString briefDescriptionWithoutUpdating();
    
private:
    friend class LLIntOffsetsExtractor;
    
    static Structure* polymorphicStructure() { return static_cast<Structure*>(reinterpret_cast<void*>(1)); }
    
    StructureID m_lastSeenStructureID;
    StructureID m_speculationFailureStructureID;
    OptionSet<ArrayProfileFlag> m_arrayProfileFlags;
    ArrayModes m_observedArrayModes { 0 };
};
static_assert(sizeof(ArrayProfile) == 16);

class UnlinkedArrayProfile {
public:
    explicit UnlinkedArrayProfile() = default;

    void update(ArrayProfile& arrayProfile)
    {
        // THREADS §5.7.5 (SPEC-jit Task 12): the unlinked profile lives on the shared
        // UnlinkedCodeBlock and can be merged with several linked profiles by several
        // mutators racily. Both merge directions are relaxed atomic ORs (monotone; lost
        // bits benign, I12). The trailing unlinked-flag store is a word-atomic
        // last-writer-wins snapshot (DidPerformFirstRunPruning intentionally dropped);
        // a stale snapshot only loses advisory bits.
        static_assert(sizeof(m_arrayProfileFlags) == sizeof(uint32_t));

        ArrayModes linkedModes = WTF::atomicLoad(&arrayProfile.m_observedArrayModes, std::memory_order_relaxed);
        ArrayModes newModes = linkedModes | WTF::atomicLoad(&m_observedArrayModes, std::memory_order_relaxed);
        WTF::atomicExchangeOr(&m_observedArrayModes, newModes, std::memory_order_relaxed);
        WTF::atomicExchangeOr(&arrayProfile.m_observedArrayModes, newModes, std::memory_order_relaxed);

        auto unlinkedFlagsSnapshot = OptionSet<ArrayProfileFlag>::fromRaw(
            WTF::atomicLoad(reinterpret_cast<uint32_t*>(&m_arrayProfileFlags), std::memory_order_relaxed));
        arrayProfile.addArrayProfileFlagsConcurrently(unlinkedFlagsSnapshot);
        auto unlinkedArrayProfileFlags = OptionSet<ArrayProfileFlag>::fromRaw(
            WTF::atomicLoad(reinterpret_cast<uint32_t*>(&arrayProfile.m_arrayProfileFlags), std::memory_order_relaxed));
        unlinkedArrayProfileFlags.remove(ArrayProfileFlag::DidPerformFirstRunPruning); // We do not propagate DidPerformFirstRunPruning.
        WTF::atomicStore(reinterpret_cast<uint32_t*>(&m_arrayProfileFlags), unlinkedArrayProfileFlags.toRaw(), std::memory_order_relaxed);
    }

private:
    ArrayModes m_observedArrayModes { 0 };
    OptionSet<ArrayProfileFlag> m_arrayProfileFlags { };
};
static_assert(sizeof(UnlinkedArrayProfile) <= 8);

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
