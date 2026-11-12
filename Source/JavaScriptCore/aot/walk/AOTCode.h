/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTBody.h"
#include "AOTBuiltinIDTypes.h"
#include "AOTCodeRole.h"
#include "AOTEffects.h"
#include "AOTFacts.h"
#include "AOTID.h"
#include "AOTOpcode.h"
#include "AOTOperandKinds.h"
#include "AOTResultRules.h"
#include "AOTSchemaRecords.h"
#include "AOTSchemaSupport.h"
#include "AOTSystemResources.h"
#include "AOTTypeKinds.h"
#include "AOTTypeNarrowing.h"
#include "AOTVector.h"
#include "AOTWire.h"
#include "IterationKind.h"
#include "TypedArrayType.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <optional>
#include <span>
#include <type_traits>
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/OSAllocator.h>
#include <wtf/OptionSet.h>
#include <wtf/PageBlock.h>
#include <wtf/PrintStream.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace Typed {

class Arrivals;
class CallingConventions;
class Code;
class CodeTextParser;
class Plan;
class Program;
class TypeArena;
class Walker;

using EdgeID = ID<struct EdgeIDTag>;
using VirtualObjectID = ID<struct VirtualObjectIDTag>;

class Reference {
public:
    enum class Kind : uint8_t { Value, Constant, Virtual, Postponed };

    static constexpr uint32_t kindShift = 30;
    static constexpr uint32_t indexMask = (1u << kindShift) - 1;

    constexpr Reference() = default;
    constexpr Reference(std::nullptr_t) { }

    static constexpr Reference value(ValueID id) { return make(Kind::Value, id.index()); }
    static constexpr Reference constant(ValueID id) { return make(Kind::Constant, id.index()); }
    static constexpr Reference virtualObject(VirtualObjectID id) { return make(Kind::Virtual, id.index()); }
    static constexpr Reference postponed(uint32_t index) { return make(Kind::Postponed, index); }
    static constexpr Reference fromBits(uint32_t bits)
    {
        Reference reference;
        reference.m_bits = bits;
        return reference;
    }

    constexpr uint32_t bits() const { return m_bits; }
    constexpr Kind kind() const { return static_cast<Kind>(m_bits >> kindShift); }
    constexpr explicit operator bool() const { return m_bits != UINT32_MAX; }
    constexpr bool isValue() const { return kind() == Kind::Value; }
    constexpr bool isConstant() const { return kind() == Kind::Constant; }
    constexpr bool isVirtual() const { return kind() == Kind::Virtual; }
    constexpr bool isPostponed() const { return kind() == Kind::Postponed && m_bits != UINT32_MAX; }
    constexpr bool isRecord() const { return isValue() || isConstant(); }

    constexpr ValueID id() const
    {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(isRecord());
        return ValueID(m_bits & indexMask);
    }
    constexpr VirtualObjectID object() const
    {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(isVirtual());
        return VirtualObjectID(m_bits & indexMask);
    }

    friend constexpr bool operator==(Reference, Reference) = default;

private:
    static constexpr Reference make(Kind kind, uint32_t index)
    {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(index < indexMask);
        return fromBits(static_cast<uint32_t>(kind) << kindShift | index);
    }

    uint32_t m_bits { UINT32_MAX };
};

static_assert(sizeof(Reference) == sizeof(uint32_t) && !Reference() && !Reference().isPostponed() && !Reference().isRecord() && !Reference().isVirtual());

static constexpr uint8_t byteOfRoomThatIsTaken = 0xa5;

template<typename T, unsigned logOfMostRecords>
class StableArray {
    WTF_MAKE_NONCOPYABLE(StableArray);
    static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>);
public:
    StableArray() = default;
    ~StableArray()
    {
        if (m_records)
            releaseAddresses(m_records, m_bytesReserved);
    }

    uint32_t size() const { return m_size; }
    bool isEmpty() const { return !m_size; }

    ALWAYS_INLINE T& operator[](uint32_t index) LIFETIME_BOUND
    {
        ASSERT(index < m_size);
        return m_records[index];
    }
    ALWAYS_INLINE const T& operator[](uint32_t index) const LIFETIME_BOUND
    {
        ASSERT(index < m_size);
        return m_records[index];
    }
    T& last() LIFETIME_BOUND { return (*this)[m_size - 1]; }
    const T& last() const LIFETIME_BOUND { return (*this)[m_size - 1]; }

    uint32_t append(const T& value)
    {
        uint32_t index = m_size;
        if (index == m_capacity) [[unlikely]]
            commitMoreRecords();
        ++m_size;
        (*this)[index] = value;
        return index;
    }
    T& appendNew() LIFETIME_BOUND
    {
        uint32_t index = m_size;
        if (index == m_capacity) [[unlikely]]
            commitMoreRecords();
        ++m_size;
        return *new (NotNull, &(*this)[index]) T;
    }
    void grow(uint32_t newSize, const T& fill)
    {
        while (m_size < newSize)
            append(fill);
    }
    void shrink(uint32_t newSize)
    {
        ASSERT(newSize <= m_size);
        m_size = newSize;
    }
    void clear() { m_size = 0; }
    void releasePages(uint32_t first, uint32_t end)
    {
#if ASSERT_ENABLED
        memset(static_cast<void*>(m_records + first), byteOfRoomThatIsTaken, sizeof(T) * (end - first));
#else
        uintptr_t begin = roundUpToMultipleOf(pageSize(), std::bit_cast<uintptr_t>(m_records + first));
        uintptr_t behind = std::bit_cast<uintptr_t>(m_records + end) & ~(static_cast<uintptr_t>(pageSize()) - 1);
        if (begin < behind) {
            OSAllocator::decommit(std::bit_cast<void*>(begin), behind - begin);
            OSAllocator::commit(std::bit_cast<void*>(begin), behind - begin, true, false);
        }
#endif
    }
    void forgetUnit()
    {
#if ASSERT_ENABLED
        if (m_size)
            memset(static_cast<void*>(m_records), byteOfRoomThatIsTaken, sizeof(T) * m_size);
#endif
        m_size = 0;
        if (m_bytesCommitted > mostBytesKept) [[unlikely]]
            decommitRest();
    }

private:
    static constexpr size_t commitGranularity = 256 * KB;
    static constexpr size_t mostBytesKept = 512 * KB;

    COLD NEVER_INLINE void reserve()
    {
        static constexpr size_t parts = 64;
        static constexpr size_t mostBytesReserved = roundUpToMultipleOf<commitGranularity>(sizeof(T) << logOfMostRecords);
        m_bytesReserved = mostBytesReserved;
        static std::atomic<bool> addressesAreUnlimited { false };
        if (addressesAreUnlimited.load(std::memory_order_relaxed))
            m_records = static_cast<T*>(tryReserveAddresses(m_bytesReserved));
        while (!m_records && m_bytesReserved > commitGranularity) {
            if (void* probe = tryReserveAddresses(m_bytesReserved * parts)) {
                releaseAddresses(probe, m_bytesReserved * parts);
                addressesAreUnlimited.store(m_bytesReserved == mostBytesReserved, std::memory_order_relaxed);
                break;
            }
            m_bytesReserved = roundUpToMultipleOf<commitGranularity>(m_bytesReserved / 2);
        }
        if (!m_records)
            m_records = static_cast<T*>(tryReserveAddresses(m_bytesReserved));
        while (!m_records && m_bytesReserved > commitGranularity) {
            m_bytesReserved = roundUpToMultipleOf<commitGranularity>(m_bytesReserved / 2);
            m_records = static_cast<T*>(tryReserveAddresses(m_bytesReserved));
        }
        if (!m_records) {
            exitIfBuildIsOutOfMemory();
            RELEASE_ASSERT_WITH_MESSAGE(m_records, "AOT: no addresses are left for the records of a unit");
        }
    }
    COLD NEVER_INLINE void commitMoreRecords()
    {
        if (!m_records)
            reserve();
        RELEASE_ASSERT_WITH_MESSAGE(m_bytesCommitted < m_bytesReserved, "AOT: a unit has more records than the process may reserve addresses for");
        OSAllocator::commit(reinterpret_cast<uint8_t*>(m_records) + m_bytesCommitted, commitGranularity, true, false);
        m_bytesCommitted += commitGranularity;
        m_capacity = static_cast<uint32_t>(m_bytesCommitted / sizeof(T));
    }

    COLD NEVER_INLINE void decommitRest()
    {
        OSAllocator::decommit(reinterpret_cast<uint8_t*>(m_records) + mostBytesKept, m_bytesCommitted - mostBytesKept);
        m_bytesCommitted = mostBytesKept;
        m_capacity = static_cast<uint32_t>(m_bytesCommitted / sizeof(T));
    }

    T* m_records { nullptr };
    uint32_t m_size { 0 };
    uint32_t m_capacity { 0 };
    size_t m_bytesReserved { 0 };
    size_t m_bytesCommitted { 0 };
};

class SideWords {
    WTF_MAKE_NONCOPYABLE(SideWords);
public:
    struct Mark {
        uint32_t segment { 0 };
        uint32_t used { 0 };
    };

    SideWords() = default;
    ~SideWords();

    ALWAYS_INLINE uint32_t* allocate(uint32_t numberOfWords)
    {
        if (numberOfWords <= m_capacityOfCurrent - m_used) [[likely]] {
            uint32_t* words = m_wordsOfCurrent + m_used;
            m_used += numberOfWords;
            return words;
        }
        return allocateInAnotherSegment(numberOfWords);
    }
    template<typename T>
    std::span<const T> copy(std::span<const T> elements)
    {
        static_assert(std::is_trivially_copyable_v<T> && sizeof(T) == sizeof(uint32_t));
        if (elements.empty())
            return { };
        uint32_t* words = allocate(static_cast<uint32_t>(elements.size()));
        uint32_t* to = words;
        const T* from = elements.data();
        size_t left = elements.size();
        for (; left >= 4; left -= 4, to += 4, from += 4)
            memcpy(to, from, 4 * sizeof(uint32_t));
        if (left & 2) {
            memcpy(to, from, 2 * sizeof(uint32_t));
            to += 2;
            from += 2;
        }
        if (left & 1)
            memcpy(to, from, sizeof(uint32_t));
        return { reinterpret_cast<const T*>(words), elements.size() };
    }

    void rewindTo(Mark);
    void clear() { rewindTo({ }); }
    void forgetUnit()
    {
        clear();
        if (!m_segments.isEmpty() && m_segments.first().capacity > wordsPerSegment) {
            fastFree(m_segments.first().words);
            m_segments.removeAt(0);
            clear();
        }
#if ASSERT_ENABLED
        for (const Segment& segment : m_segments)
            memset(segment.words, byteOfRoomThatIsTaken, static_cast<size_t>(segment.capacity) * sizeof(uint32_t));
#endif
    }

private:
    static constexpr uint32_t wordsPerSegment = 1u << 14;

    struct Segment {
        uint32_t* words { nullptr };
        uint32_t capacity { 0 };
    };

    COLD NEVER_INLINE uint32_t* allocateInAnotherSegment(uint32_t numberOfWords);

    Vector<Segment, 4> m_segments;
    uint32_t m_current { 0 };
    uint32_t m_used { 0 };
    uint32_t* m_wordsOfCurrent { nullptr };
    uint32_t m_capacityOfCurrent { 0 };
};

template<typename T, uint32_t inlineCapacity>
class ShortList {
    static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T> && sizeof(T) == sizeof(uint32_t) && alignof(T) == alignof(uint32_t));
public:
    ShortList() = default;
    ShortList(const ShortList& other)
    {
        if (other.liesInline()) [[likely]] {
            m_size = other.m_size;
            memcpy(m_inlineWords, other.m_inlineWords, sizeof(m_inlineWords));
            return;
        }
        replaceAll(other.span());
    }
    ShortList(ShortList&& other) { take(other); }
    ~ShortList()
    {
        if (!liesInline()) [[unlikely]]
            fastFree(m_words);
    }

    ShortList& operator=(const ShortList& other)
    {
        if (this != &other)
            replaceAll(other.span());
        return *this;
    }
    ShortList& operator=(ShortList&& other)
    {
        if (this == &other)
            return *this;
        if (!liesInline()) [[unlikely]]
            fastFree(m_words);
        take(other);
        return *this;
    }

    uint32_t size() const { return m_size; }
    bool isEmpty() const { return !m_size; }
    std::span<const T> span() const LIFETIME_BOUND { return { m_words, m_size }; }
    T* begin() LIFETIME_BOUND { return m_words; }
    T* end() LIFETIME_BOUND { return m_words + m_size; }
    const T* begin() const LIFETIME_BOUND { return m_words; }
    const T* end() const LIFETIME_BOUND { return m_words + m_size; }

    T& operator[](uint32_t index) LIFETIME_BOUND
    {
        RELEASE_ASSERT(index < m_size);
        return m_words[index];
    }
    const T& operator[](uint32_t index) const LIFETIME_BOUND
    {
        RELEASE_ASSERT(index < m_size);
        return m_words[index];
    }

    void reserveCapacity(uint32_t capacity)
    {
        if (capacity > m_capacity) [[unlikely]]
            grow(capacity);
    }
    void append(T word)
    {
        if (m_size == m_capacity) [[unlikely]]
            grow(m_size + 1);
        m_words[m_size++] = word;
    }
    void shrink(uint32_t size)
    {
        ASSERT(size <= m_size);
        m_size = size;
    }
    ALWAYS_INLINE T* resizeWithoutWords(uint32_t size) LIFETIME_BOUND
    {
        m_size = 0;
        reserveCapacity(size);
        m_size = size;
        return m_words;
    }
    ALWAYS_INLINE void resetToWordsOfOnes(uint32_t size)
    {
        if (size > inlineCapacity) [[unlikely]] {
            memset(static_cast<void*>(resizeWithoutWords(size)), 0xFF, size * sizeof(T));
            return;
        }
        m_size = size;
        memset(static_cast<void*>(m_words), 0xFF, inlineCapacity * sizeof(T));
    }
    ALWAYS_INLINE void resetToFewWordsOfOnes(uint32_t size)
    {
        ASSERT(size <= inlineCapacity);
        m_size = size;
        memset(static_cast<void*>(m_words), 0xFF, inlineCapacity * sizeof(T));
    }
    ALWAYS_INLINE void resetToWordsAtHand(const std::array<T, inlineCapacity>& words, uint32_t size)
    {
        ASSERT(size <= inlineCapacity);
        m_size = size;
        memcpy(static_cast<void*>(m_words), words.data(), inlineCapacity * sizeof(T));
    }
    ALWAYS_INLINE void resetTo(const T* words, uint32_t size)
    {
        T* own = size > inlineCapacity ? resizeWithoutWords(size) : m_words;
        m_size = size;
        for (; size >= 4; size -= 4, own += 4, words += 4)
            memcpy(own, words, 4 * sizeof(T));
        if (size & 2) {
            memcpy(own, words, 2 * sizeof(T));
            own += 2;
            words += 2;
        }
        if (size & 1)
            *own = *words;
    }
    void replaceFrom(uint32_t first, std::span<const T> words, T filler)
    {
        uint32_t size = first + static_cast<uint32_t>(words.size());
        if (size > m_capacity) [[unlikely]] {
            replaceFromInNewMemory(first, words, filler);
            return;
        }
        for (uint32_t index = m_size; index < first; ++index)
            m_words[index] = filler;
        if (!words.empty())
            memmove(m_words + first, words.data(), words.size_bytes());
        m_size = size;
    }

private:
    bool liesInline() const { return m_words == inlineWords(); }
    T* inlineWords() { return reinterpret_cast<T*>(m_inlineWords); }
    const T* inlineWords() const { return reinterpret_cast<const T*>(m_inlineWords); }

    void replaceAll(std::span<const T> words)
    {
        T* own = resizeWithoutWords(static_cast<uint32_t>(words.size()));
        if (!words.empty())
            memcpy(own, words.data(), words.size_bytes());
    }
    void take(ShortList& other)
    {
        m_size = std::exchange(other.m_size, 0);
        if (other.liesInline()) [[likely]] {
            m_words = inlineWords();
            m_capacity = inlineCapacity;
            memcpy(m_inlineWords, other.m_inlineWords, sizeof(m_inlineWords));
            return;
        }
        m_words = std::exchange(other.m_words, other.inlineWords());
        m_capacity = std::exchange(other.m_capacity, inlineCapacity);
    }
    COLD NEVER_INLINE void grow(uint32_t capacity)
    {
        capacity = std::max(capacity, m_capacity * 2);
        T* grown = static_cast<T*>(fastMalloc(static_cast<size_t>(capacity) * sizeof(T)));
        memcpy(grown, m_words, m_size * sizeof(T));
        if (!liesInline())
            fastFree(m_words);
        m_words = grown;
        m_capacity = capacity;
    }
    COLD NEVER_INLINE void replaceFromInNewMemory(uint32_t first, std::span<const T> words, T filler)
    {
        uint32_t size = first + static_cast<uint32_t>(words.size());
        uint32_t capacity = std::max(size, m_capacity * 2);
        T* grown = static_cast<T*>(fastMalloc(static_cast<size_t>(capacity) * sizeof(T)));
        uint32_t kept = std::min(m_size, first);
        memcpy(grown, m_words, kept * sizeof(T));
        for (uint32_t index = kept; index < first; ++index)
            grown[index] = filler;
        if (!words.empty())
            memcpy(grown + first, words.data(), words.size_bytes());
        if (!liesInline())
            fastFree(m_words);
        m_words = grown;
        m_capacity = capacity;
        m_size = size;
    }

    T* m_words { inlineWords() };
    uint32_t m_size { 0 };
    uint32_t m_capacity { inlineCapacity };
    uint32_t m_inlineWords[inlineCapacity];
};

class Instruction {
public:
    using OperandID = ValueID;

    static constexpr uint32_t noPosition = UINT32_MAX;
    static constexpr uint32_t numberOfInlineWords = 4;

    std::span<const ValueID> operands() const LIFETIME_BOUND
    {
        if (m_hasSideWords)
            return { reinterpret_cast<const ValueID*>(sideWords() + numberOfSidePayloadWords()), m_words[3] };
        return { reinterpret_cast<const ValueID*>(m_words.data()), m_numberOfInlineOperands };
    }
    uint32_t numberOfOperands() const { return m_hasSideWords ? m_words[3] : m_numberOfInlineOperands; }
    uint32_t numberOfNamedOperands() const { return m_hasSideWords ? m_words[2] & maxNumberOfNamedOperands : m_numberOfInlineOperands; }
    uint32_t numberOfVariadicOperands() const { return numberOfOperands() - numberOfNamedOperands(); }
    ValueID operand(uint32_t operandIndex) const
    {
        std::span<const ValueID> all = operands();
        RELEASE_ASSERT(operandIndex < all.size());
        return all[operandIndex];
    }
    ValueID child(unsigned index) const { return index < numberOfNamedOperands() ? operand(index) : ValueID(); }
    std::span<const ValueID> variadicOperands() const LIFETIME_BOUND { return operands().subspan(numberOfNamedOperands()); }

    void forEachOperand(NOESCAPE const Invocable<void(ValueID, uint32_t operandIndex)> auto& functor) const
    {
        uint32_t operandIndex = 0;
        for (ValueID operand : operands()) {
            if (operand)
                functor(operand, operandIndex);
            ++operandIndex;
        }
    }
    void forEachChild(NOESCAPE const Invocable<void(ValueID)> auto& functor) const
    {
        for (ValueID operand : operands()) {
            if (operand)
                functor(operand);
        }
    }

    std::span<const uint32_t> payload() const LIFETIME_BOUND
    {
        if (m_hasSideWords)
            return { sideWords(), numberOfSidePayloadWords() };
        return { m_words.data() + m_numberOfInlineOperands, m_numberOfInlinePayloadWords };
    }
    uint32_t payloadWordAt(unsigned word) const
    {
        std::span<const uint32_t> words = payload();
        if (word < words.size()) [[likely]]
            return words[word];
        RELEASE_ASSERT(word < numberOfInlinePayloadWords);
        return absentPayloadWord;
    }
    uint64_t payloadWordPairAt(unsigned word) const { return static_cast<uint64_t>(payloadWordAt(word + 1)) << 32 | payloadWordAt(word); }
    std::span<const uint32_t> payloadListFrom(unsigned firstWord) const LIFETIME_BOUND
    {
        std::span<const uint32_t> words = payload();
        if (firstWord >= words.size())
            return { };
        return words.subspan(firstWord);
    }
    template<typename Element>
    std::span<const Element> payloadListOf(unsigned firstWord) const LIFETIME_BOUND
    {
        static_assert(std::is_trivially_copyable_v<Element> && alignof(Element) <= alignof(uint32_t) && !(sizeof(Element) % sizeof(uint32_t)));
        return spanReinterpretCast<const Element>(payloadListFrom(firstWord));
    }

    bool hasPosition() const { return position != noPosition; }
    Origin origin() const { return { position, inlineFrame, !!isTailCall, !!isPartOfConstruction }; }

private:
    std::array<uint32_t, numberOfInlineWords> m_words { };
public:
    uint32_t position { noPosition };
    uint32_t inlineFrame : 24 { 0 };
    uint32_t flags : 8 { 0 };
    uint8_t isTailCall : 1 { false };
    uint8_t isPartOfConstruction : 1 { false };
private:
    uint8_t m_hasSideWords : 1 { false };
    uint8_t m_numberOfInlineOperands : 3 { 0 };
    uint8_t m_numberOfInlinePayloadWords : 2 { 0 };

protected:
    Instruction() = default;

private:
    friend class Code;

    static constexpr uint32_t maxNumberOfNamedOperands = 15;

    const uint32_t* sideWords() const { return std::bit_cast<const uint32_t*>(std::array<uint32_t, 2> { m_words[0], m_words[1] }); }
    uint32_t numberOfSidePayloadWords() const { return m_words[2] >> 4; }
};

struct Operation : Instruction {
    Representation representation { Representation::Unchosen };
    Opcode opcode { Opcode::Undefined };
    TypeID type;
    ValueID previous;
    uint32_t blockIndex : 32 - EffectTraits::numberOfBits = noBlock;
    uint32_t traitBits : EffectTraits::numberOfBits = 0;

    static constexpr uint32_t noBlock = (1u << (32 - EffectTraits::numberOfBits)) - 1;

    BlockID block() const { return blockIndex == noBlock ? BlockID() : BlockID(blockIndex); }
    EffectTraits traits() const { return EffectTraits::fromBits(traitBits); }

    ALWAYS_INLINE OperandKind operandKind(uint32_t operandIndex) const { return operandKindOf(opcode, operandIndex); }
    OperandUses operandUses(uint32_t operandIndex) const { return operandUsesOf(opcode, operandIndex); }

#include "AOTSchemaOperationGetters.inc"
};

static_assert(sizeof(Operation) <= 48 && std::is_trivially_copyable_v<Operation>);

class ArgumentsOfEdge {
public:
    ArgumentsOfEdge() = default;
    std::span<const ValueID> span() const { return m_records.span(); }

private:
    friend class Code;
    friend class CodeTextParser;
    friend class Walker;
    Vector<ValueID, 8> m_records;
    BlockID m_heldFor;
};

struct Edge {
    BlockID from;
    BlockID to;

    bool isOpen() const { return !to; }
    std::span<const ValueID> arguments() const LIFETIME_BOUND { return { m_arguments, m_numberOfArguments }; }
    EdgeID nextInto() const { return m_nextInto; }

private:
    friend class Code;

    const ValueID* m_arguments { nullptr };
    uint32_t m_numberOfArguments { 0 };
    EdgeID m_nextInto;
};

struct Terminator : Instruction {
    TerminatorKind kind { TerminatorKind::Unreachable };
    bool isSet { false };
    EdgeID firstEdge;
    uint32_t numberOfEdges { 0 };

    ALWAYS_INLINE OperandKind operandKind(uint32_t operandIndex) const { return operandKindOf(kind, operandIndex); }
    OperandUses operandUses(uint32_t operandIndex) const { return operandUsesOf(kind, operandIndex); }

    bool isConditionalBranch() const { return kind == TerminatorKind::Branch || kind == TerminatorKind::TestAndBranch; }
    bool hasCases() const { return !numberOfFixedTargetsOf(kind); }
    uint32_t numberOfCases() const { return hasCases() ? numberOfEdges - 1 : 0; }

    EdgeID edge(uint32_t targetIndex) const
    {
        RELEASE_ASSERT(targetIndex < numberOfEdges);
        return EdgeID(firstEdge.index() + targetIndex);
    }
    EdgeID next() const { RELEASE_ASSERT(kind == TerminatorKind::Jump); return edge(0); }
    EdgeID taken() const { RELEASE_ASSERT(isConditionalBranch()); return edge(0); }
    EdgeID notTaken() const { RELEASE_ASSERT(isConditionalBranch()); return edge(1); }
    EdgeID caseEdge(uint32_t caseIndex) const { RELEASE_ASSERT(caseIndex < numberOfCases()); return edge(caseIndex); }
    EdgeID otherwise() const { RELEASE_ASSERT(hasCases()); return edge(numberOfEdges - 1); }
    EdgeID done() const { RELEASE_ASSERT(kind == TerminatorKind::TryCallInline); return edge(0); }
    EdgeID miss() const { RELEASE_ASSERT(kind == TerminatorKind::TryCallInline); return edge(1); }

#include "AOTSchemaTerminatorGetters.inc"
};

enum class BeliefAheadOfEdges : uint8_t { NoEdgeIsAwaited, Nothing, Listed, GivenUp };

struct Block {
    Terminator terminator;
    EdgeID handler;
    ValueID subject;
    BlockRole role { BlockRole::Ordinary };
    BlockFrequency frequency { BlockFrequency::Normal };
    bool isInLoop { false };
    bool isLoopHead { false };
    bool isEntry { false };
    bool meetsStepWrittenByHand { false };
    bool keepsSitesThoughRare { false };
    bool isSealed { false };
    BeliefAheadOfEdges beliefAheadOfEdges { BeliefAheadOfEdges::NoEdgeIsAwaited };

    bool isHandlerBlock() const { return role != BlockRole::Ordinary; }
    bool hasEnded() const { return terminator.isSet; }
    std::span<const ValueID> parameters() const LIFETIME_BOUND { return { m_parameters, m_numberOfParameters }; }
    static constexpr size_t mostParametersSearchedFromFront = 32;
    ALWAYS_INLINE size_t placeOfParameter(ValueID parameter) const
    {
        if (m_numberOfParameters > mostParametersSearchedFromFront) [[unlikely]]
            return placeAmongManyParameters(parameter);
        return std::ranges::find(parameters(), parameter) - parameters().begin();
    }
    EdgeID firstEdgeIn() const { return m_firstEdgeIn; }

private:
    friend class Code;
    friend class Sweep;

    size_t placeAmongManyParameters(ValueID) const;

    const ValueID* m_parameters { nullptr };
    uint32_t m_numberOfParameters { 0 };
    EdgeID m_firstEdgeIn;
    EdgeID m_lastEdgeIn;
    ValueID m_last;
    uint32_t m_firstInOrder { 0 };
    uint32_t m_numberInOrder { 0 };
};

struct EntryOfCode {
    BlockID block;
};

struct FactUse {
    ValueID record;
    FactKind kind { };
    FactSubject::Kind subjectKind { FactSubject::Kind::Variable };
    uint32_t subject { UINT32_MAX };
    uint32_t index { 0 };
    TypeID type;
    PropertyKeyID key;
};

enum class SweepMark : uint8_t {
    IsNeeded = 1 << 0,
    ReleasesOperand = 1 << 1,
    LaterBarrierCovers = 1 << 2,
    LeavesOwedError = 1 << 3,
    PollsForNothing = 1 << 4,
    LaterFenceCovers = 1 << 5,
    OwnerIsYoung = 1 << 7,
};

enum class FateOfPoll : uint8_t {
    LeavesHandler,
    IsDropped,
};

struct PollWithFate {
    ValueID poll;
    FateOfPoll fate { FateOfPoll::IsDropped };
    ValueID unlessKept;
};

class DraftWords {
public:
    using OperandID = Reference;

    static constexpr uint32_t noPosition = UINT32_MAX;

    std::span<const Reference> operands() const LIFETIME_BOUND { return m_operands.span(); }
    uint32_t numberOfOperands() const { return m_operands.size(); }
    uint32_t numberOfNamedOperands() const { return m_numberOfNamedOperands; }
    uint32_t numberOfVariadicOperands() const { return numberOfOperands() - m_numberOfNamedOperands; }
    Reference operand(uint32_t operandIndex) const { return m_operands[operandIndex]; }
    Reference child(unsigned index) const { return index < m_numberOfNamedOperands ? m_operands[index] : Reference(); }
    void setOperand(uint32_t operandIndex, Reference reference) { m_operands[operandIndex] = reference; }

    std::span<const Reference> variadicOperands() const LIFETIME_BOUND { return operands().subspan(m_numberOfNamedOperands); }
    void reserveVariadicOperands(uint32_t count) { m_operands.reserveCapacity(m_numberOfNamedOperands + count); }
    void appendVariadicOperand(Reference reference) { m_operands.append(reference); }
    void setVariadicOperands(std::span<const Reference> references) { m_operands.replaceFrom(m_numberOfNamedOperands, references, Reference()); }

    void forEachOperand(NOESCAPE const Invocable<void(Reference, uint32_t operandIndex)> auto& functor) const
    {
        for (uint32_t operandIndex = 0; operandIndex < m_operands.size(); ++operandIndex) {
            if (m_operands[operandIndex])
                functor(m_operands[operandIndex], operandIndex);
        }
    }

    std::span<const uint32_t> payload() const LIFETIME_BOUND { return m_payload.span(); }
    uint32_t payloadWordAt(unsigned word) const
    {
        if (word < m_payload.size()) [[likely]]
            return m_payload[word];
        RELEASE_ASSERT(word < numberOfInlinePayloadWords);
        return absentPayloadWord;
    }
    void setPayloadWordAt(unsigned word, uint32_t value) { m_payload[word] = value; }
    uint64_t payloadWordPairAt(unsigned word) const { return static_cast<uint64_t>(payloadWordAt(word + 1)) << 32 | payloadWordAt(word); }
    void setPayloadWordPairAt(unsigned word, uint64_t value)
    {
        setPayloadWordAt(word, static_cast<uint32_t>(value));
        setPayloadWordAt(word + 1, static_cast<uint32_t>(value >> 32));
    }
    std::span<const uint32_t> payloadListFrom(unsigned firstWord) const LIFETIME_BOUND
    {
        if (firstWord >= m_payload.size())
            return { };
        return m_payload.span().subspan(firstWord);
    }
    void setPayloadListFrom(unsigned firstWord, std::span<const uint32_t> words) { m_payload.replaceFrom(firstWord, words, absentPayloadWord); }
    template<typename Element>
    std::span<const Element> payloadListOf(unsigned firstWord) const LIFETIME_BOUND
    {
        static_assert(std::is_trivially_copyable_v<Element> && alignof(Element) <= alignof(uint32_t) && !(sizeof(Element) % sizeof(uint32_t)));
        return spanReinterpretCast<const Element>(payloadListFrom(firstWord));
    }
    template<typename Element>
    void setPayloadListOf(unsigned firstWord, std::span<const Element> elements)
    {
        static_assert(std::is_trivially_copyable_v<Element> && alignof(Element) <= alignof(uint32_t) && !(sizeof(Element) % sizeof(uint32_t)));
        setPayloadListFrom(firstWord, spanReinterpretCast<const uint32_t>(elements));
    }

    bool hasPosition() const { return position != noPosition; }
    Origin origin() const { return { position, inlineFrame, isTailCall, isPartOfConstruction }; }
    void setOrigin(Origin origin)
    {
        position = origin.position;
        inlineFrame = origin.inlineFrame;
        isTailCall = origin.isTailCall;
        isPartOfConstruction = origin.isPartOfConstruction;
    }

    uint32_t position { noPosition };
    uint32_t inlineFrame { 0 };
    uint8_t flags { 0 };
    bool isTailCall { false };
    bool isPartOfConstruction { false };

protected:
    DraftWords() = default;

    ALWAYS_INLINE void resetOperandsAndPayload(uint32_t numberOfNamedOperands, std::span<const uint32_t> defaultPayload)
    {
        static_assert(Reference().bits() == UINT32_MAX);
        flags = 0;
        m_numberOfNamedOperands = numberOfNamedOperands;
        m_operands.resetToWordsOfOnes(numberOfNamedOperands);
        m_payload.resetTo(defaultPayload.data(), static_cast<uint32_t>(defaultPayload.size()));
    }
    ALWAYS_INLINE void resetOperandsAndPayload(const FreshWordsOfOperation& fresh)
    {
        if (fresh.numberOfPayloadWords > numberOfFreshPayloadWordsAtHand || fresh.numberOfNamedOperands > numberOfOperandsKeptInDraft) [[unlikely]] {
            resetOperandsAndPayloadOfManyWords(fresh);
            return;
        }
        flags = 0;
        m_numberOfNamedOperands = fresh.numberOfNamedOperands;
        m_operands.resetToFewWordsOfOnes(fresh.numberOfNamedOperands);
        m_payload.resetToWordsAtHand(fresh.firstPayloadWords, fresh.numberOfPayloadWords);
    }
    NEVER_INLINE void resetOperandsAndPayloadOfManyWords(const FreshWordsOfOperation& fresh)
    {
        resetOperandsAndPayload(fresh.numberOfNamedOperands, { fresh.payload, fresh.numberOfPayloadWords });
    }
    void copyWordsOf(const Instruction&, NOESCAPE const Invocable<Reference(ValueID)> auto& referenceOf);

private:
    static constexpr uint32_t numberOfOperandsKeptInDraft = 8;
    ShortList<Reference, numberOfOperandsKeptInDraft> m_operands;
    ShortList<uint32_t, numberOfFreshPayloadWordsAtHand> m_payload;
    uint32_t m_numberOfNamedOperands { 0 };
};

void DraftWords::copyWordsOf(const Instruction& instruction, NOESCAPE const Invocable<Reference(ValueID)> auto& referenceOf)
{
    setOrigin(instruction.origin());
    flags = instruction.flags;
    m_numberOfNamedOperands = instruction.numberOfNamedOperands();
    m_operands.shrink(0);
    for (ValueID operand : instruction.operands())
        m_operands.append(operand ? referenceOf(operand) : Reference());
    m_payload.replaceFrom(0, instruction.payload(), absentPayloadWord);
}

struct SteppedWithCounter {
    ValueID stepped;
    ValueID counter;
    ValueID steppedStart;
    ValueID counterStart;
    bool counterRises { false };

    bool operator==(const SteppedWithCounter&) const = default;
};

struct GroundsOfPosition {
    ValueID view;
    ValueID position;
    ValueID length;
    ValueID isBelow;
    bool isBelowResult { true };
    ValueID isFromZeroUp;
    bool isNonNegativeResult { true };
    ValueID counter;
    ValueID start;
    ValueID stepNonNegativeCheck;
    ValueID limitWithinLengthCheck;
    ValueID loweredCounter;
    ValueID loweredFrom;
    bool loweringLoopIsClosed { false };
    SteppedWithCounter steppedBeside { };
    ValueID counterTest;
    bool counterTestResult { true };
};

class PassedTest {
public:
    struct Side {
        ValueID test;
        bool testResult { true };
    };
    std::span<const Side> sides() const LIFETIME_BOUND { return std::span(m_sides).first(m_numberOfSides); }
    const std::optional<GroundsOfPosition>& groundsOfPosition() const LIFETIME_BOUND { return m_groundsOfPosition; }
    PassedTest with(const PassedTest& other) const
    {
        PassedTest both = *this;
        for (const Side& side : other.sides())
            both.add(side);
        if (!both.m_groundsOfPosition)
            both.m_groundsOfPosition = other.m_groundsOfPosition;
        return both;
    }

private:
    friend class Walker;
    friend struct GroundsWrittenInTests;
    PassedTest() = default;
    PassedTest(ValueID test, bool testResult) { add({ test, testResult }); }
    void add(const Side& added)
    {
        for (const Side& side : sides()) {
            if (side.test == added.test && side.testResult == added.testResult)
                return;
        }
        RELEASE_ASSERT(m_numberOfSides < m_sides.size());
        m_sides[m_numberOfSides++] = added;
    }

    std::array<Side, 4> m_sides;
    uint8_t m_numberOfSides { 0 };
    std::optional<GroundsOfPosition> m_groundsOfPosition;
};

struct Draft : DraftWords {
    Representation representation { Representation::Unchosen };
    Opcode opcode { Opcode::Undefined };
    ExpectedType expected;
    bool resultIsReturnedAtOnce { false };
    bool restIsDefinedBehind { false };
    bool standsForNoValueOfInput { false };
    ValueID inputValueGiven;
    std::optional<PassedTest> testsPassed;

    explicit Draft(Opcode newOpcode) { become(newOpcode); }
    Draft(Opcode newOpcode, Origin origin)
    {
        setOrigin(origin);
        become(newOpcode);
    }

    ALWAYS_INLINE void become(Opcode newOpcode)
    {
        opcode = newOpcode;
        representation = Representation::Unchosen;
        expected = { };
        resultIsReturnedAtOnce = false;
        restIsDefinedBehind = false;
        standsForNoValueOfInput = false;
        inputValueGiven = { };
        testsPassed = std::nullopt;
        resetOperandsAndPayload(freshWordsOf(newOpcode));
    }

    ALWAYS_INLINE OperandKind operandKind(uint32_t operandIndex) const { return operandKindOf(opcode, operandIndex); }
    OperandUses operandUses(uint32_t operandIndex) const { return operandUsesOf(opcode, operandIndex); }
    void restOn(const PassedTest& passed)
    {
        RELEASE_ASSERT(passed.groundsOfPosition() && passed.groundsOfPosition()->view == operand(0).id() && passed.groundsOfPosition()->position == operand(1).id());
        testsPassed = testsPassed ? testsPassed->with(passed) : passed;
    }

#include "AOTSchemaOperationGetters.inc"
#include "AOTSchemaOperationSetters.inc"
};

struct TerminatorDraft : DraftWords {
    TerminatorKind kind { TerminatorKind::Unreachable };

    explicit TerminatorDraft(TerminatorKind newKind) { become(newKind); }
    TerminatorDraft(TerminatorKind newKind, Origin origin)
    {
        become(newKind);
        setOrigin(origin);
    }

    void become(TerminatorKind newKind)
    {
        kind = newKind;
        resetOperandsAndPayload(numberOfNamedOperandsOf(newKind), defaultPayloadOf(newKind));
    }

    ALWAYS_INLINE OperandKind operandKind(uint32_t operandIndex) const { return operandKindOf(kind, operandIndex); }
    OperandUses operandUses(uint32_t operandIndex) const { return operandUsesOf(kind, operandIndex); }

#include "AOTSchemaTerminatorGetters.inc"
#include "AOTSchemaTerminatorSetters.inc"
};

struct LiveAtSuspension {
    ValueID suspension;
    uint32_t first { 0 };
    uint32_t count { 0 };
};

class Code {
    WTF_MAKE_NONCOPYABLE(Code);
public:
    Code() = default;

    void reset(CodeUnit);

    enum class FrequenciesOfBlocks : uint8_t { AsGiven, AllNormal, DrawnBySeed };
    void takeFrequenciesOfBlocks(FrequenciesOfBlocks taken, uint32_t seed)
    {
        m_frequenciesOfBlocks = taken;
        m_seedOfFrequencies = seed;
    }
    uint32_t numberOfBlocksPlacedOtherwise() const { return m_numberOfBlocksPlacedOtherwise; }

    CodeUnit unit() const { return m_unit; }
    FunctionID function() const { return m_unit.function; }
    CodeRole role() const { return m_unit.role; }

    uint32_t numberOfValues() const { return m_operations.size(); }
    uint32_t numberOfBlocks() const { return m_blocks.size(); }
    uint32_t numberOfEdges() const { return m_edges.size(); }

    ALWAYS_INLINE const Operation& at(ValueID id) const LIFETIME_BOUND { return m_operations[id.index()]; }
    ALWAYS_INLINE const Block& at(BlockID id) const LIFETIME_BOUND { return m_blocks[id.index()]; }
    ALWAYS_INLINE const Edge& at(EdgeID id) const LIFETIME_BOUND { return m_edges[id.index()]; }

    bool isConstant(ValueID id) const { return !at(id).block(); }
    BlockID blockOf(ValueID id) const { return at(id).block(); }
    ValueID lastOperationOf(BlockID id) const { return at(id).m_last; }

    std::span<const InlineFrame> inlineFrames() const LIFETIME_BOUND { return m_inlineFrames.span(); }
    std::span<const EntryOfCode> entries() const LIFETIME_BOUND { return m_entries.span(); }
    uint32_t numberOfFrameWords() const { return m_numberOfFrameWords; }

    std::span<const FactUse> factUses() const LIFETIME_BOUND { return m_factUses.span(); }
    std::span<const PollWithFate> pollsWithFates() const LIFETIME_BOUND { return m_pollsWithFates.span(); }

    bool hasHandlers() const { return m_hasHandlers; }
    bool hasBoxesForHandlers() const { return m_hasBoxesForHandlers; }
    void noteBoxForHandler() { m_hasBoxesForHandlers = true; }
    bool hasSuspensions() const { return m_hasSuspensions; }
    bool hasLoops() const { return m_hasLoops; }
    bool holdsFrameCells() const { return m_holdsFrameCells; }
    bool holdsCallThatPlanHasNotCounted() const { return m_holdsCallThatPlanHasNotCounted; }
    bool callsOfFunctionNeverCollect() const { return m_callsOfFunctionNeverCollect; }
    bool readsNumbersOutOfBytes() const { return m_readsNumbersOutOfBytes || m_opcodesOfValues.contains(Opcode::GetDataViewElement); }
    bool callsItself() const { return m_callsItself; }
    bool isEntryThatWalksBody() const { return m_isEntryThatWalksBody; }
    bool mayHoldReturnedCalls() const { return m_mayHoldReturnedCalls; }
    bool has(Mechanism mechanism, const Facts& facts) const { return facts.mechanisms().contains(mechanism) && !m_mechanismsWithheld.contains(mechanism); }
    const OpcodeSet& opcodesOfValues() const LIFETIME_BOUND { return m_opcodesOfValues; }

    class AllIncomingEdges {
    public:
        void forEach(NOESCAPE const Invocable<void(const Edge&)> auto& functor) const { m_code.forEachIncomingEdgeUnsealed(m_block, functor); }

    private:
        friend class Code;
        AllIncomingEdges(const Code& code, BlockID block)
            : m_code(code)
            , m_block(block)
        {
        }

        const Code& m_code;
        BlockID m_block;
    };
    AllIncomingEdges seal(BlockID);
    AllIncomingEdges allIncomingEdges(BlockID block) const { return { *this, block }; }
    void forEachIncomingEdgeUnsealed(BlockID block, NOESCAPE const Invocable<void(const Edge&)> auto& functor) const
    {
        for (EdgeID edge = at(block).firstEdgeIn(); edge; edge = at(edge).nextInto())
            functor(at(edge));
    }
    void forEachEdgeOf(BlockID block, NOESCAPE const Invocable<void(EdgeID, const Edge&)> auto& functor) const
    {
        const Terminator& terminator = at(block).terminator;
        for (uint32_t index = 0; index < terminator.numberOfEdges; ++index)
            functor(terminator.edge(index), at(terminator.edge(index)));
        if (EdgeID handler = handlerOf(block))
            functor(handler, at(handler));
    }

    Wire::SourceLocation locationOf(const Program&, Origin) const;

    bool isSwept() const { return m_isSwept; }
    enum class SourcesOfNames : bool { StandAnywhere, StandEarlier };
    template<const OpcodeSet& names, SourcesOfNames sources = SourcesOfNames::StandAnywhere, unsigned namesGoneDownInLine = 4>
    ALWAYS_INLINE ValueID valueBehindNames(ValueID name) const
    {
        if constexpr (sources == SourcesOfNames::StandEarlier) {
            if (!name)
                return name;
        }
        const Operation& operation = at(name);
        if (!names.contains(operation.opcode))
            return name;
        ASSERT(operation.operandUses(0).contains(OperandUse::Returns));
        if constexpr (namesGoneDownInLine) {
            ValueID source = operation.operand(0);
            if constexpr (sources == SourcesOfNames::StandEarlier) {
                if (source.index() >= name.index())
                    return { };
            }
            return valueBehindNames<names, sources, namesGoneDownInLine - 1>(source);
        } else
            return valueBehindManyNames(name, names, sources);
    }
    ValueID valueBehindManyNames(ValueID, const OpcodeSet&, SourcesOfNames) const;
    ValueID valueBehindRowOfNarrowed(ValueID name) const;
    bool isNeeded(ValueID id) const { return marksOf(id).contains(SweepMark::IsNeeded); }
    OptionSet<SweepMark> marksOf(ValueID id) const { return OptionSet<SweepMark>::fromRaw(m_marks[id.index()]); }
    bool barrierIsCountedOn(ValueID store) const { return m_storesWhoseBarriersAreCountedOn.get(store.index()); }
    bool someBarrierIsCountedOn() const { return !m_storesWhoseBarriersAreCountedOn.isEmpty(); }
    void noteBarrierIsCountedOn(ValueID store) { m_storesWhoseBarriersAreCountedOn.set(store.index()); }
    bool isReached(BlockID id) const { return m_reachedBlocks.get(id.index()); }
    std::span<const ValueID> valuesOf(const Block& block) const LIFETIME_BOUND { return m_order.span().subspan(block.m_firstInOrder, block.m_numberInOrder); }
    std::span<const ValueID> valuesOf(BlockID id) const LIFETIME_BOUND { return valuesOf(at(id)); }
    uint32_t numberOfOperationsInOrder() const { return m_numberOfOperationsInOrder; }
    EdgeID handlerOf(BlockID id) const { return m_isSwept && m_blocksWithoutThrows.get(id.index()) ? EdgeID() : at(id).handler; }
    uint8_t useCountOf(ValueID id) const { return m_useCounts[id.index()]; }
    ValueID onlyUserOf(ValueID id) const { return m_onlyUsers[id.index()]; }
    ValueID parameterFedBy(ValueID id) const { return m_parametersFed[id.index()]; }
    std::span<const ValueID> liveAt(ValueID suspension) const LIFETIME_BOUND;
    std::span<const LiveAtSuspension> suspensions() const LIFETIME_BOUND { return m_suspensions.span(); }

    BlockID addBlock(BlockRole = BlockRole::Ordinary, BlockFrequency = BlockFrequency::Normal);
    BlockID addBlockBehindEdge(EdgeID, BlockFrequency, bool isInLoop);
    void setLoopMarks(BlockID, bool isInLoop, bool isLoopHead);
    void markRareBlockThatKeepsSites(BlockID);
    void noteBeliefAheadOfEdges(BlockID, BeliefAheadOfEdges);
    void makeBlocksRare(uint32_t first, uint32_t end);
    void makeBlockRare(BlockID);
    void makeBlockNormal(BlockID);
    void makeBlockUnlikely(BlockID, bool thoughRare = false);
    void setSubject(BlockID, ValueID);
    void markMeetingOfStepWrittenByHand(BlockID);
    void setParameters(BlockID, std::span<const ValueID>);
    ValueID appendParameter(BlockID, TypeID, Representation, Origin);
    void setTypeOfJoinParameter(ValueID, TypeID);
    void setCheckedTypeOfProof(ValueID check, TypeID);
    void makeTypeTestAskInFull(ValueID);
    void makeTestAskInFull(ValueID);
    void letJoinFailWhereItsReaderFails(ValueID);
    void turnFinishIntoUnusedString(ValueID);
    void turnBranchIntoJump(BlockID, EdgeID lastOnWay);
    void leadJumpPastBlockThatJumps(BlockID);
    void leaveCheckOfProofToTestBehind(ValueID);
    void bearListWithStorage(ValueID, ArrayStorage);
    void letListRememberStorageAtSite(ValueID, bool takesAnyValueAtFirst = false);
    void letListForgetStorageAtSite(ValueID);
    ValueID append(BlockID, const Draft&, TypeID proven, EffectTraits);
    void noteEffectsBelieved(ValueID, const Effects&);
    void noteTestsPassedFor(ValueID, const PassedTest&);
    const PassedTest* testsPassedFor(ValueID) const;
    void noteSteppedWithCounter(const SteppedWithCounter&);
    std::span<const SteppedWithCounter> steppedWithCounters() const LIFETIME_BOUND { return m_steppedWithCounters.span(); }
    bool keepsTestsPassed() const { return m_keepsTestsPassed; }
    void setKeepsTestsPassed(bool keeps) { m_keepsTestsPassed = keeps; }
    void setMarksLoops(bool marks) { m_marksLoops = marks; }
    const Effects* effectsBelievedOf(ValueID) const;
    void noteThatOperationWritesWhatTurnHasMade(ValueID);
    bool writesWhatTurnHasMade(ValueID) const;
    struct TestOfGuardInFrontOfCopy {
        ValueID test;
        FunctionID callee;
        uint32_t positionOfCallee { 0 };
    };
    void noteTestOfGuardInFrontOfCopy(TestOfGuardInFrontOfCopy noted) { m_testsOfGuardsInFrontOfCopies.append(noted); }
    std::span<const TestOfGuardInFrontOfCopy> testsOfGuardsInFrontOfCopies() const LIFETIME_BOUND { return m_testsOfGuardsInFrontOfCopies.span(); }
    struct InputOfHoleReadAsNaN {
        ValueID read;
        FunctionID function;
        ValueID written;
    };
    void noteInputOfHoleReadAsNaN(InputOfHoleReadAsNaN noted)
    {
        while (!m_inputsOfHolesReadAsNaN.isEmpty() && m_inputsOfHolesReadAsNaN.last().read.index() >= noted.read.index())
            m_inputsOfHolesReadAsNaN.removeLast();
        m_readsAnsweredByHolesReadAsNaN.removeAllMatching([&](const InputOfHoleReadAsNaN& answered) {
            return answered.read.index() >= noted.read.index();
        });
        m_inputsOfHolesReadAsNaN.append(noted);
    }
    std::span<const InputOfHoleReadAsNaN> inputsOfHolesReadAsNaN() const LIFETIME_BOUND { return m_inputsOfHolesReadAsNaN.span(); }
    void noteReadAnsweredByHoleReadAsNaN(InputOfHoleReadAsNaN answered) { m_readsAnsweredByHolesReadAsNaN.append(answered); }
    std::span<const InputOfHoleReadAsNaN> readsAnsweredByHolesReadAsNaN() const LIFETIME_BOUND { return m_readsAnsweredByHolesReadAsNaN.span(); }
    struct CellsMadeForOneOperation {
        ValueID first;
        ValueID second;
    };
    void noteCellsMadeForOneOperation(CellsMadeForOneOperation noted)
    {
        while (!m_cellsMadeForOneOperation.isEmpty() && m_cellsMadeForOneOperation.last().second.index() >= noted.second.index())
            m_cellsMadeForOneOperation.removeLast();
        m_cellsMadeForOneOperation.append(noted);
    }
    std::span<const CellsMadeForOneOperation> cellsMadeForOneOperation() const LIFETIME_BOUND { return m_cellsMadeForOneOperation.span(); }
    void recordSyntheticVariableRead(ValueID read)
    {
        while (!m_syntheticVariableReads.isEmpty() && m_syntheticVariableReads.last().index() >= read.index())
            m_syntheticVariableReads.removeLast();
        m_syntheticVariableReads.append(read);
    }
    std::span<const ValueID> syntheticVariableReads() const LIFETIME_BOUND { return m_syntheticVariableReads.span(); }
    void setMergedObjects(const BitVector& parameters) { m_mergedObjects = parameters; }
    bool isMergedObject(ValueID parameter) const { return m_mergedObjects.get(parameter.index()); }
    void noteValueThatFoldedArithmeticGives(ValueID given) { m_valuesThatFoldedArithmeticGives.append(given); }
    std::span<const ValueID> valuesThatFoldedArithmeticGives() const LIFETIME_BOUND { return m_valuesThatFoldedArithmeticGives.span(); }
    ValueID appendConversion(BlockID, Opcode, ValueID operand, Representation, TypeID proven, Origin);
    ValueID addConstant(const Draft&, TypeID proven);
    ValueID addCopyOfConstant(ValueID constant, Representation);
    void end(BlockID, const TerminatorDraft&, uint32_t numberOfEdges);
    EdgeID endWithJump(BlockID, Origin);
    void endWithBranch(BlockID, ValueID condition, Origin);
    void endWithJump(BlockID, Origin, BlockID to, const ArgumentsOfEdge&);
    void endWithBranch(BlockID, ValueID condition, Origin, BlockID taken, BlockID notTaken);
    EdgeID addHandlerEdge(BlockID);
    void closeEdge(EdgeID, BlockID to, const ArgumentsOfEdge&);
    uint32_t addInlineFrame(const InlineFrame&);
    uint16_t addEntry(BlockID);
    void addFactUse(const FactUse&);
    void addFateOfPoll(ValueID poll, FateOfPoll, ValueID unlessKept = { });
    void reserveFrameWords(uint32_t numberOfWords) { m_numberOfFrameWords = std::max(m_numberOfFrameWords, numberOfWords); }
    void noteCallThatPlanHasNotCounted() { m_holdsCallThatPlanHasNotCounted = true; }
    void noteThatEntryWalksBody() { m_isEntryThatWalksBody = true; }
    void withhold(Mechanisms mechanisms) { m_mechanismsWithheld = mechanisms; }
    void noteThatCallsOfFunctionNeverCollect() { m_callsOfFunctionNeverCollect = true; }

    struct OpenBlock {
        BlockID block;
        ValueID last;
        bool hadEnded { false };
        bool hadHandler { false };
    };
    struct Mark {
        uint32_t numberOfOperations { 0 };
        uint32_t numberOfBlocks { 0 };
        uint32_t numberOfEdges { 0 };
        uint32_t numberOfInlineFrames { 0 };
        uint32_t numberOfEntries { 0 };
        uint32_t numberOfFrameWords { 0 };
        uint32_t numberOfFactUses { 0 };
        uint32_t numberOfEffectsBelieved { 0 };
        uint32_t numberOfOperationsThatWriteWhatTurnHasMade { 0 };
        uint32_t numberOfTestsPassed { 0 };
        uint32_t numberOfSteppedCounters { 0 };
        uint32_t numberOfTestsOfGuardsInFrontOfCopies { 0 };
        uint32_t numberOfInputsOfHolesReadAsNaN { 0 };
        uint32_t numberOfReadsAnsweredByHolesReadAsNaN { 0 };
        uint32_t numberOfValuesThatFoldedArithmeticGives { 0 };
        uint32_t numberOfPollsWithFates { 0 };
        uint32_t numberOfChangesToOlderParts { 0 };
        uint32_t numberOfOlderBlocksEnded { 0 };
        OpcodeSet opcodesOfValues;
        bool hasHandlers { false };
        bool hasSuspensions { false };
        bool hasLoops { false };
        bool holdsFrameCells { false };
        bool holdsCallThatPlanHasNotCounted { false };
        bool callsOfFunctionNeverCollect { false };
        bool readsNumbersOutOfBytes { false };
        bool callsItself { false };
    };
    Mark mark() const;
    OpenBlock stateOf(BlockID) const;
    void abandonBackTo(const Mark&, std::span<const OpenBlock> openBlocks);
    bool isAbandoned(EdgeID edge) const { return m_graveyard && at(edge).from == m_graveyard; }
    bool isAbandoned(ValueID record) const
    {
        if (m_recordsAbandoned.isEmpty() || record.index() < m_recordsAbandoned.first().first)
            return false;
        auto behind = std::ranges::upper_bound(m_recordsAbandoned, record.index(), { }, &std::pair<uint32_t, uint32_t>::first);
        return record.index() < (behind - 1)->second;
    }
    void releaseAbandonedBlocks();
    void keepChangesToOlderParts(bool keeps)
    {
        m_keepsChangesToOlderParts = keeps;
        if (!keeps) {
            m_changesToOlderParts.shrink(0);
            m_olderBlocksEnded.shrink(0);
        }
    }

private:
    friend class Sweep;

    void fill(Instruction&, const DraftWords&);
    void appendEdgeInto(BlockID from, BlockID to, std::span<const ValueID> arguments);
    void layClosureInFrame(ValueID, LoanOfClosure);
    void layObjectInFrame(ValueID);
    BlockFrequency frequencyTakenFor(uint32_t block, BlockFrequency given);
    bool loopMarkIsDrawnAway(uint32_t block) const;

    CodeUnit m_unit;
    StableArray<Operation, 26> m_operations;
    StableArray<Block, 24> m_blocks;
    StableArray<Edge, 25> m_edges;
    SideWords m_sideWords;
    Vector<InlineFrame, 4> m_inlineFrames;
    Vector<EntryOfCode, 2> m_entries;
    Vector<FactUse> m_factUses;
    Vector<std::pair<ValueID, Effects>> m_effectsBelieved;
    Vector<ValueID> m_operationsThatWriteWhatTurnHasMade;
    Vector<std::pair<ValueID, PassedTest>> m_testsPassed;
    Vector<SteppedWithCounter> m_steppedWithCounters;
    bool m_keepsTestsPassed { false };
    bool m_marksLoops { true };
    Vector<TestOfGuardInFrontOfCopy> m_testsOfGuardsInFrontOfCopies;
    Vector<InputOfHoleReadAsNaN> m_inputsOfHolesReadAsNaN;
    Vector<InputOfHoleReadAsNaN> m_readsAnsweredByHolesReadAsNaN;
    Vector<CellsMadeForOneOperation> m_cellsMadeForOneOperation;
    Vector<ValueID> m_syntheticVariableReads;
    BitVector m_mergedObjects;
    Vector<ValueID> m_valuesThatFoldedArithmeticGives;
    Vector<PollWithFate> m_pollsWithFates;
    struct ChangeToOlderPart {
        EdgeID closed;
        BlockID entered;
        EdgeID lastEdgeInBefore;
        ValueID changed;
        Operation recordBefore;
        BlockID marked;
        BlockFrequency frequencyBefore { BlockFrequency::Normal };
        bool wasInLoop { false };
        bool wasSealed { false };
    };
    Vector<ChangeToOlderPart> m_changesToOlderParts;
    struct OlderBlockEnded {
        BlockID block;
        Terminator before;
    };
    Vector<OlderBlockEnded> m_olderBlocksEnded;
    mutable uint32_t m_numberOfBlocksAtNewestMark { 0 };
    void keepMarksOfOlderBlock(uint32_t index)
    {
        if (m_keepsChangesToOlderParts && index < m_numberOfBlocksAtNewestMark)
            m_changesToOlderParts.append(ChangeToOlderPart { { }, { }, { }, { }, { }, BlockID(index), m_blocks[index].frequency, m_blocks[index].isInLoop, m_blocks[index].isSealed });
    }
    void setTerminator(BlockID blockID, Block& block, const Terminator& terminator)
    {
        if (m_keepsChangesToOlderParts && blockID.index() < m_numberOfBlocksAtNewestMark)
            m_olderBlocksEnded.append({ blockID, block.terminator });
        block.terminator = terminator;
    }
    Vector<std::pair<uint32_t, uint32_t>, 4> m_recordsAbandoned;
    struct PartsAbandoned {
        std::pair<uint32_t, uint32_t> operations;
        std::pair<uint32_t, uint32_t> blocks;
        std::pair<uint32_t, uint32_t> edges;
    };
    Vector<PartsAbandoned, 4> m_partsAbandoned;
    bool m_keepsChangesToOlderParts { false };
    BlockID m_graveyard;
    OpcodeSet m_opcodesOfValues;
    uint32_t m_numberOfFrameWords { 0 };
    bool m_hasHandlers { false };
    bool m_hasBoxesForHandlers { false };
    bool m_hasSuspensions { false };
    bool m_hasLoops { false };
    bool m_holdsFrameCells { true };
    bool m_holdsCallThatPlanHasNotCounted { false };
    bool m_callsOfFunctionNeverCollect { false };
    bool m_readsNumbersOutOfBytes { false };
    bool m_callsItself { false };
    bool m_isEntryThatWalksBody { false };
    bool m_mayHoldReturnedCalls { false };
    Mechanisms m_mechanismsWithheld;
    FrequenciesOfBlocks m_frequenciesOfBlocks { FrequenciesOfBlocks::AsGiven };
    uint32_t m_seedOfFrequencies { 0 };
    uint32_t m_numberOfBlocksPlacedOtherwise { 0 };

    bool m_isSwept { false };
    Vector<uint8_t> m_marks;
    Vector<uint8_t> m_useCounts;
    Vector<ValueID> m_onlyUsers;
    mutable UncheckedKeyHashMap<uint32_t, uint32_t> m_valuesBehindRowsOfNarrowed;
    Vector<ValueID> m_parametersFed;
    Vector<ValueID> m_order;
    uint32_t m_numberOfOperationsInOrder { 0 };
    BitVector m_reachedBlocks;
    BitVector m_blocksWithoutThrows;
    BitVector m_storesWhoseBarriersAreCountedOn;
    Vector<LiveAtSuspension> m_suspensions;
    Vector<ValueID> m_liveValues;
};

struct CodeTextContext {
    const Program& program;
    TypeArena& types;
    const Plan* plan { nullptr };
    const Facts* facts { nullptr };
};

void printCode(PrintStream&, const CodeTextContext&, const Code&);
String parseCode(const CodeTextContext&, StringView text, Code&);

template<typename Test>
TypeID narrowOperandByTest(const TypingContext& context, const Test& test, OperandTypes typesOfTestOperands, uint32_t operandIndex, EdgeSide side)
{
    return typeOfEdgeResult(context, test, typesOfTestOperands, operandIndex, side);
}
inline ValueID valueThatTestSpeaksOf(const Code& code, ValueID test, uint32_t operandIndex)
{
    const Operation& operation = code.at(test);
    if (provesOf(operation.opcode) == Proof::None)
        return test;
    return operandIndex < operation.numberOfOperands() ? operation.operand(operandIndex) : ValueID();
}
inline constexpr OpcodeSet operationsThatStandAmongNarrowedValues = conversionsOfForm | OpcodeSet { Opcode::CheckProof };
inline constexpr OpcodeSet readsThatNarrowTheirHolders { Opcode::GetSlot, Opcode::GetSlotOrUndefined };
inline std::optional<uint64_t> lengthThatListIsMadeWith(const Code& code, const Operation& made)
{
    if (made.opcode == Opcode::NewArray)
        return made.elementsOperands().size() + made.holes().size();
    if (made.opcode != Opcode::NewArrayLike || made.sourceOperand() || !made.lengthOperand())
        return std::nullopt;
    const Operation& length = code.at(made.lengthOperand());
    if (length.opcode != Opcode::Integer || length.integer() < 0 || length.integer() > static_cast<int64_t>(UINT32_MAX))
        return std::nullopt;
    return static_cast<uint64_t>(length.integer());
}
inline bool hasTestedForHole(const Operation& read)
{
    return read.opcode == Opcode::GetArrayElement && !read.isPresent() && !read.readsHoleAsNaN();
}
template<typename Read>
ReadOfEmptySlot whatReadOfEmptySlotGives(const Read& read)
{
    return opcodeOf(read) == Opcode::GetSlotOrUndefined ? ReadOfEmptySlot::GivesUndefined : ReadOfEmptySlot::IsRuledOut;
}
template<typename Read>
TypeID narrowHolderOfRead(const TypingContext& context, const Read& read, TypeID holder, TypeID valueRead)
{
    if (context.level == TypeLevel::Proven && testsItsOwnGuess(read))
        return holder;
    return narrowHolderByFieldValue(context, holder, read.key(), valueRead, whatReadOfEmptySlotGives(read));
}
template<typename Test, typename Read>
TypeID narrowHolderByTest(const TypingContext& context, const Test& test, OperandTypes typesOfTestOperands, uint32_t operandIndex, const Read& read, TypeID holder, EdgeSide side)
{
    return narrowHolderOfRead(context, read, holder, typeOfEdgeResult(context, test, typesOfTestOperands, operandIndex, side));
}
template<typename Read>
TypeID narrowHolderByCondition(const TypingContext& context, TypeID condition, const Read& read, TypeID holder, EdgeSide side)
{
    return narrowHolderOfRead(context, read, holder, narrowByTruthiness(context, condition, side));
}
template<typename Switch>
TypeID narrowSubjectByCase(const TypingContext& context, const Switch& terminator, TypeID subject, std::optional<uint32_t> caseIndex)
{
    return typeOfCaseResult(context, terminator, subject, caseIndex.value_or(UINT32_MAX));
}
template<typename Switch, typename Read>
TypeID narrowHolderOfSubjectByCase(const TypingContext& context, const Switch& terminator, TypeID subject, const Read& read, TypeID holder, std::optional<uint32_t> caseIndex)
{
    return narrowHolderOfRead(context, read, holder, narrowSubjectByCase(context, terminator, subject, caseIndex));
}
TypeID narrowOtherResultByTest(const TypingContext&, const Code&, ValueID test, ValueID value, EdgeSide);
TypeID typeOfNarrowed(const TypingContext&, const Code&, ValueID narrowed);
TypeID typeOfJoinParameter(TypeArena&, std::span<const EdgeID> edgesThatReachJoin, NOESCAPE const ScopedLambda<TypeID(EdgeID)>& typeArrivingOn);

String verifyCode(const CodeTextContext&, const Code&, InternalErrorKind* kindOfBrokenRule = nullptr);
String verifySweptCode(const CodeTextContext&, const Code&, InternalErrorKind* kindOfBrokenRule = nullptr);
String verifyFrameCellsOfSweptCode(const CodeTextContext&, const Code&, InternalErrorKind* kindOfBrokenRule = nullptr);
class DominatorsOfCode {
    WTF_MAKE_NONCOPYABLE(DominatorsOfCode);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(DominatorsOfCode);
public:
    DominatorsOfCode(const Code&, std::span<const BlockID> reachedBlocksInReversePostorder);

    bool dominates(BlockID dominator, BlockID block) const { return m_firstNumberOfSubtree[block.index()] - m_firstNumberOfSubtree[dominator.index()] < m_sizeOfSubtree[dominator.index()]; }

private:
    Vector<uint32_t> m_sizeOfSubtree;
    Vector<uint32_t> m_firstNumberOfSubtree;
};
String verifyTypesOfCode(const TypingContext&, const CallingConventions&, const Arrivals&, const Code&);

extern const OpcodeSet opcodesWithoutEmitterInCode;
inline const OpcodeSet& opcodesWithoutEmitter() { return opcodesWithoutEmitterInCode; }
bool readsNumberOutOfBytes(const Operation&);
bool readsNumberOrNothingOutOfBytes(const Operation&);
bool handsOnBitsOfNaN(const Operation&);
bool takesAnyNaN(const Operation& user, uint32_t operandIndex);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
