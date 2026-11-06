/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTCompactList.h"
#include "AOTID.h"
#include "AOTOpcode.h"
#include "AOTSchemaRecords.h"
#include "AOTSchemaSupport.h"
#include "AOTTypeKinds.h"
#include "AOTVector.h"
#include "IterationKind.h"
#include "TypedArrayType.h"
#include <algorithm>
#include <array>
#include <bit>
#include <optional>
#include <span>
#include <type_traits>
#include <wtf/Hasher.h>
#include <wtf/OptionSet.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

class Instruction {
public:
    static constexpr uint32_t noPosition = UINT32_MAX;

    Instruction(const Instruction& other)
        : position(other.position)
        , inlineFrame(other.inlineFrame)
        , flags(other.flags)
        , isTailCall(other.isTailCall)
        , isPartOfConstruction(other.isPartOfConstruction)
    {
        copyWordsFrom(other);
    }
    Instruction(Instruction&& other)
        : position(other.position)
        , inlineFrame(other.inlineFrame)
        , flags(other.flags)
        , isTailCall(other.isTailCall)
        , isPartOfConstruction(other.isPartOfConstruction)
    {
        moveWordsFrom(other);
    }
    ~Instruction() { freeWords(); }
    Instruction& operator=(const Instruction& other)
    {
        if (this == &other)
            return *this;
        freeWords();
        position = other.position;
        inlineFrame = other.inlineFrame;
        flags = other.flags;
        isTailCall = other.isTailCall;
        isPartOfConstruction = other.isPartOfConstruction;
        copyWordsFrom(other);
        return *this;
    }
    Instruction& operator=(Instruction&& other)
    {
        if (this == &other)
            return *this;
        freeWords();
        position = other.position;
        inlineFrame = other.inlineFrame;
        flags = other.flags;
        isTailCall = other.isTailCall;
        isPartOfConstruction = other.isPartOfConstruction;
        moveWordsFrom(other);
        return *this;
    }

private:
    static constexpr uint32_t numberOfInlineWords = 3;
    static constexpr uint32_t maxNumberOfNamedOperands = 15;
    static constexpr uint32_t maxNumberOfPayloadWords = (1 << 28) - 1;
    static constexpr uint32_t outOfLineHeaderSize = 2;

    struct OutOfLineWords {
        uint32_t* storage;
        uint32_t numberOfPayloadWords;
        uint32_t numberOfNamedOperands;

        uint32_t& operandCapacity() const { return storage[0]; }
        uint32_t& numberOfOperands() const { return storage[1]; }
        uint32_t* payload() const { return storage + outOfLineHeaderSize; }
        uint32_t* operands() const { return payload() + numberOfPayloadWords; }
    };

    std::array<uint32_t, numberOfInlineWords> m_words;
public:
    uint32_t position { noPosition };
    uint32_t inlineFrame : 24 { 0 };
    uint32_t flags : 8 { 0 };
    // The operation is a call in tail position that returns here, or stands for a part of one. A stack trace leaves out the frame that makes it.
    uint8_t isTailCall : 1 { false };
    // The operation is an expression that constructs, or stands for a part of one. Its position is the callee's, and call sites show where the expression starts.
    uint8_t isPartOfConstruction : 1 { false };
private:
    uint8_t m_numberOfInlinePayloadWords : 2 { 0 };
    uint8_t m_hasOutOfLineWords : 1 { false };
    uint8_t m_numberOfInlineOperands : 3 { 0 };

public:
    std::span<const ValueID> operands() const LIFETIME_BOUND
    {
        if (m_hasOutOfLineWords) {
            OutOfLineWords words = outOfLineWords();
            return { reinterpret_cast<const ValueID*>(words.operands()), words.numberOfOperands() };
        }
        return { reinterpret_cast<const ValueID*>(m_words.data()), m_numberOfInlineOperands };
    }
    std::span<ValueID> mutableOperands() LIFETIME_BOUND
    {
        std::span<const ValueID> operands = this->operands();
        return { const_cast<ValueID*>(operands.data()), operands.size() };
    }

    ValueID child(unsigned index) const { return index < numberOfNamedOperands() ? operand(index) : ValueID(); }

    uint32_t numberOfNamedOperands() const { return m_hasOutOfLineWords ? outOfLineWords().numberOfNamedOperands : m_numberOfInlineOperands; }
    uint32_t numberOfOperands() const { return m_hasOutOfLineWords ? outOfLineWords().numberOfOperands() : m_numberOfInlineOperands; }
    uint32_t numberOfVariadicOperands() const { return numberOfOperands() - numberOfNamedOperands(); }

    ValueID operand(uint32_t operandIndex) const
    {
        if (operandIndex < m_numberOfInlineOperands) [[likely]]
            return ValueID(m_words[operandIndex]);
        RELEASE_ASSERT(m_hasOutOfLineWords);
        OutOfLineWords words = outOfLineWords();
        RELEASE_ASSERT(operandIndex < words.numberOfOperands());
        return ValueID(words.operands()[operandIndex]);
    }

    void setOperand(uint32_t operandIndex, ValueID id)
    {
        if (operandIndex < m_numberOfInlineOperands) [[likely]] {
            m_words[operandIndex] = id.index();
            return;
        }
        RELEASE_ASSERT(m_hasOutOfLineWords);
        OutOfLineWords words = outOfLineWords();
        RELEASE_ASSERT(operandIndex < words.numberOfOperands());
        words.operands()[operandIndex] = id.index();
    }

    std::span<const ValueID> variadicOperands() const LIFETIME_BOUND
    {
        if (!m_hasOutOfLineWords)
            return { };
        return operands().subspan(outOfLineWords().numberOfNamedOperands);
    }
    std::span<ValueID> mutableVariadicOperands() LIFETIME_BOUND
    {
        std::span<const ValueID> operands = variadicOperands();
        return { const_cast<ValueID*>(operands.data()), operands.size() };
    }
    void reserveVariadicOperands(uint32_t count)
    {
        if (count)
            reserveOperands(numberOfNamedOperands() + count);
    }
    void appendVariadicOperand(ValueID id)
    {
        uint32_t count = numberOfOperands();
        if (!m_hasOutOfLineWords || count == outOfLineWords().operandCapacity())
            reserveOperands(count + std::max<uint32_t>(2, count / 2));
        OutOfLineWords words = outOfLineWords();
        words.operands()[count] = id.index();
        words.numberOfOperands() = count + 1;
    }

    void setVariadicOperands(std::span<const ValueID> operands)
    {
        if (operands.empty() && !m_hasOutOfLineWords)
            return;
        Vector<ValueID, 8> copy(operands);
        uint32_t count = numberOfNamedOperands() + static_cast<uint32_t>(copy.size());
        reserveOperands(count);
        outOfLineWords().numberOfOperands() = count;
        std::ranges::copy(copy, mutableVariadicOperands().begin());
    }

    void removeVariadicOperand(uint32_t variadicIndex)
    {
        std::span<ValueID> operands = mutableVariadicOperands();
        RELEASE_ASSERT(variadicIndex < operands.size());
        std::ranges::copy(operands.subspan(variadicIndex + 1), operands.begin() + variadicIndex);
        --outOfLineWords().numberOfOperands();
    }

    std::optional<uint32_t> variadicOperandIndex(uint32_t operandIndex) const
    {
        uint32_t numberOfNamedOperands = this->numberOfNamedOperands();
        if (operandIndex < numberOfNamedOperands)
            return std::nullopt;
        return operandIndex - numberOfNamedOperands;
    }

    template<typename Functor>
    void forEachOperand(const Functor& functor) const
    {
        uint32_t operandIndex = 0;
        for (const ValueID operand : operands()) {
            if (operand)
                functor(operand, operandIndex);
            ++operandIndex;
        }
    }

    template<typename Functor>
    void forEachOperand(const Functor& functor)
    {
        uint32_t operandIndex = 0;
        for (ValueID& operand : mutableOperands()) {
            if (operand)
                functor(operand, operandIndex);
            ++operandIndex;
        }
    }

    template<typename Functor>
    void forEachChild(const Functor& functor) const
    {
        for (const ValueID operand : operands()) {
            if (operand)
                functor(operand);
        }
    }

    template<typename Functor>
    void forEachChild(const Functor& functor)
    {
        for (ValueID& operand : mutableOperands()) {
            if (operand)
                functor(operand);
        }
    }

    bool hasPosition() const { return position != noPosition; }
    Origin origin() const { return { position, inlineFrame, !!isTailCall, !!isPartOfConstruction }; }
    void setOrigin(Origin origin)
    {
        position = origin.position;
        inlineFrame = origin.inlineFrame;
        isTailCall = origin.isTailCall;
        isPartOfConstruction = origin.isPartOfConstruction;
    }

    uint32_t payloadWordAt(unsigned word) const
    {
        if (word < m_numberOfInlinePayloadWords) [[likely]]
            return m_words[numberOfInlineWords - 1 - word];
        if (m_hasOutOfLineWords) {
            OutOfLineWords words = outOfLineWords();
            if (word < words.numberOfPayloadWords)
                return words.payload()[word];
        }
        RELEASE_ASSERT(word < numberOfInlinePayloadWords);
        return absentPayloadWord;
    }
    void setPayloadWordAt(unsigned word, uint32_t value)
    {
        if (word < m_numberOfInlinePayloadWords) [[likely]] {
            m_words[numberOfInlineWords - 1 - word] = value;
            return;
        }
        RELEASE_ASSERT(m_hasOutOfLineWords);
        OutOfLineWords words = outOfLineWords();
        RELEASE_ASSERT(word < words.numberOfPayloadWords);
        words.payload()[word] = value;
    }
    uint64_t payloadWordPairAt(unsigned word) const { return static_cast<uint64_t>(payloadWordAt(word + 1)) << 32 | payloadWordAt(word); }
    void setPayloadWordPairAt(unsigned word, uint64_t value)
    {
        setPayloadWordAt(word, static_cast<uint32_t>(value));
        setPayloadWordAt(word + 1, static_cast<uint32_t>(value >> 32));
    }

    std::span<const uint32_t> payloadListFrom(unsigned firstWord) const LIFETIME_BOUND
    {
        size_t numberOfPayloadWords = this->numberOfPayloadWords();
        RELEASE_ASSERT(firstWord >= numberOfInlinePayloadWords && firstWord <= std::max<size_t>(numberOfPayloadWords, numberOfInlinePayloadWords));
        if (firstWord >= numberOfPayloadWords)
            return { };
        return { outOfLineWords().payload() + firstWord, numberOfPayloadWords - firstWord };
    }
    std::span<uint32_t> mutablePayloadListFrom(unsigned firstWord) LIFETIME_BOUND
    {
        std::span<const uint32_t> list = payloadListFrom(firstWord);
        return { const_cast<uint32_t*>(list.data()), list.size() };
    }
    void setPayloadListFrom(unsigned firstWord, std::span<const uint32_t> words)
    {
        if (words.empty() && payloadListFrom(firstWord).empty())
            return;
        Vector<uint32_t, 16> copy(words);
        size_t numberOfPayloadWords = firstWord + copy.size();
        RELEASE_ASSERT(numberOfPayloadWords <= maxNumberOfPayloadWords);
        if (!m_hasOutOfLineWords || numberOfPayloadWords != outOfLineWords().numberOfPayloadWords)
            reallocateOutOfLine(numberOfOperands(), static_cast<uint32_t>(numberOfPayloadWords));
        std::ranges::copy(copy, outOfLineWords().payload() + firstWord);
    }

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

    std::array<uint32_t, numberOfInlinePayloadWords> inlinePayload() const
    {
        if (m_hasOutOfLineWords) [[unlikely]] {
            OutOfLineWords words = outOfLineWords();
            uint32_t count = words.numberOfPayloadWords;
            const uint32_t* payload = words.payload();
            return { count > 0 ? payload[0] : absentPayloadWord, count > 1 ? payload[1] : absentPayloadWord, count > 2 ? payload[2] : absentPayloadWord };
        }
        uint32_t count = m_numberOfInlinePayloadWords;
        return { count > 0 ? m_words[2] : absentPayloadWord, count > 1 ? m_words[1] : absentPayloadWord, count > 2 ? m_words[0] : absentPayloadWord };
    }
    std::span<const uint32_t> outOfLinePayload() const LIFETIME_BOUND
    {
        if (!m_hasOutOfLineWords)
            return { };
        OutOfLineWords words = outOfLineWords();
        if (words.numberOfPayloadWords <= numberOfInlinePayloadWords)
            return { };
        return { words.payload() + numberOfInlinePayloadWords, words.numberOfPayloadWords - numberOfInlinePayloadWords };
    }

    bool hasSamePayloadAs(const Instruction& other) const { return flags == other.flags && inlinePayload() == other.inlinePayload() && std::ranges::equal(outOfLinePayload(), other.outOfLinePayload()); }

    uint64_t hashOfWords() const
    {
        constexpr uint64_t multiplier = 0x9e3779b97f4a7c15ull;
        if (!m_hasOutOfLineWords) [[likely]] {
            uint64_t first = static_cast<uint64_t>(m_words[1]) << 32 | m_words[0];
            uint64_t second = static_cast<uint64_t>(flags << 8 | m_numberOfInlineOperands << 4 | m_numberOfInlinePayloadWords) << 32 | m_words[2];
            uint64_t hash = (first ^ 0x243f6a8885a308d3ull) * multiplier;
            hash = ((hash ^ hash >> 29) ^ second) * multiplier;
            return hash ^ hash >> 32;
        }
        OutOfLineWords words = outOfLineWords();
        uint32_t numberOfWords = words.numberOfPayloadWords + words.numberOfOperands();
        uint64_t hash = (static_cast<uint64_t>(flags) << 32 | words.numberOfPayloadWords) * multiplier;
        for (uint32_t index = 0; index < numberOfWords; ++index) {
            hash = (hash ^ words.payload()[index]) * multiplier;
            hash ^= hash >> 29;
        }
        return hash ^ words.numberOfNamedOperands;
    }

    void addPayloadTo(Hasher& hasher) const
    {
        add(hasher, static_cast<uint16_t>(flags));
        for (uint32_t word : inlinePayload())
            add(hasher, word);
        std::span<const uint32_t> outOfLinePayload = this->outOfLinePayload();
        add(hasher, static_cast<uint32_t>(outOfLinePayload.size()));
        for (uint32_t word : outOfLinePayload)
            add(hasher, word);
    }

protected:
    Instruction()
        : m_words { }
    {
    }

    void resetOperandsAndPayload(uint32_t numberOfNamedOperands, std::span<const uint32_t> defaultPayload)
    {
        RELEASE_ASSERT(numberOfNamedOperands <= maxNumberOfNamedOperands);
        uint32_t numberOfPayloadWords = static_cast<uint32_t>(defaultPayload.size());
        freeWords();
        flags = 0;
        if (numberOfNamedOperands + numberOfPayloadWords <= numberOfInlineWords) {
            m_hasOutOfLineWords = false;
            m_numberOfInlineOperands = numberOfNamedOperands;
            m_numberOfInlinePayloadWords = numberOfPayloadWords;
            m_words.fill(UINT32_MAX);
            for (uint32_t word = 0; word < numberOfPayloadWords; ++word)
                m_words[numberOfInlineWords - 1 - word] = defaultPayload[word];
            return;
        }
        OutOfLineWords words = allocateWords(numberOfNamedOperands, numberOfNamedOperands, numberOfNamedOperands, numberOfPayloadWords);
        std::ranges::copy(defaultPayload, words.payload());
        setOutOfLineWords(words);
    }

private:
    static_assert(numberOfInlinePayloadWords == 3 && absentPayloadWord == UINT32_MAX && !ValueID(UINT32_MAX));

    OutOfLineWords outOfLineWords() const { return { std::bit_cast<uint32_t*>(std::array<uint32_t, 2> { m_words[0], m_words[1] }), m_words[2] >> 4, m_words[2] & maxNumberOfNamedOperands }; }
    uint32_t numberOfPayloadWords() const { return m_hasOutOfLineWords ? outOfLineWords().numberOfPayloadWords : m_numberOfInlinePayloadWords; }

    static size_t allocationSize(uint32_t operandCapacity, uint32_t numberOfPayloadWords) { return roundUpToMultipleOf<2>(outOfLineHeaderSize + static_cast<size_t>(numberOfPayloadWords) + operandCapacity); }
    static uint32_t operandCapacityFor(size_t numberOfAllocatedWords, uint32_t numberOfPayloadWords) { return static_cast<uint32_t>(std::min<size_t>(numberOfAllocatedWords - outOfLineHeaderSize - numberOfPayloadWords, UINT32_MAX)); }

    static OutOfLineWords allocateWords(uint32_t numberOfNamedOperands, uint32_t numberOfOperands, uint32_t operandCapacity, uint32_t numberOfPayloadWords)
    {
        size_t numberOfWords = allocationSize(operandCapacity, numberOfPayloadWords);
        OutOfLineWords words { static_cast<uint32_t*>(fastMalloc(numberOfWords * sizeof(uint32_t))), numberOfPayloadWords, numberOfNamedOperands };
        memset(words.storage, 0xff, numberOfWords * sizeof(uint32_t));
        words.operandCapacity() = operandCapacityFor(numberOfWords, numberOfPayloadWords);
        words.numberOfOperands() = numberOfOperands;
        return words;
    }

    void setOutOfLineWords(OutOfLineWords words)
    {
        m_hasOutOfLineWords = true;
        m_numberOfInlineOperands = 0;
        m_numberOfInlinePayloadWords = 0;
        auto pointer = std::bit_cast<std::array<uint32_t, 2>>(words.storage);
        m_words = { pointer[0], pointer[1], words.numberOfPayloadWords << 4 | words.numberOfNamedOperands };
    }

    void reallocateOutOfLine(uint32_t operandCapacity, uint32_t numberOfPayloadWords)
    {
        std::span<const ValueID> oldOperands = operands();
        OutOfLineWords words = allocateWords(numberOfNamedOperands(), static_cast<uint32_t>(oldOperands.size()), operandCapacity, numberOfPayloadWords);
        for (uint32_t word = 0, end = std::min(this->numberOfPayloadWords(), numberOfPayloadWords); word < end; ++word)
            words.payload()[word] = payloadWordAt(word);
        memcpy(words.operands(), oldOperands.data(), oldOperands.size_bytes());
        freeWords();
        setOutOfLineWords(words);
    }

    void reserveOperands(uint32_t capacity)
    {
        if (!m_hasOutOfLineWords) {
            reallocateOutOfLine(capacity, m_numberOfInlinePayloadWords);
            return;
        }
        OutOfLineWords words = outOfLineWords();
        if (capacity <= words.operandCapacity())
            return;
        size_t numberOfWords = allocationSize(capacity, words.numberOfPayloadWords);
        words.storage = static_cast<uint32_t*>(fastRealloc(words.storage, numberOfWords * sizeof(uint32_t)));
        words.operandCapacity() = operandCapacityFor(numberOfWords, words.numberOfPayloadWords);
        setOutOfLineWords(words);
    }

    void copyLayoutFrom(const Instruction& other)
    {
        m_numberOfInlineOperands = other.m_numberOfInlineOperands;
        m_numberOfInlinePayloadWords = other.m_numberOfInlinePayloadWords;
        m_hasOutOfLineWords = other.m_hasOutOfLineWords;
    }

    void copyWordsFrom(const Instruction& other)
    {
        if (!other.m_hasOutOfLineWords) {
            copyLayoutFrom(other);
            m_words = other.m_words;
            return;
        }
        OutOfLineWords words = other.outOfLineWords();
        OutOfLineWords copy = allocateWords(words.numberOfNamedOperands, words.numberOfOperands(), words.numberOfOperands(), words.numberOfPayloadWords);
        memcpy(copy.payload(), words.payload(), (static_cast<size_t>(words.numberOfPayloadWords) + words.numberOfOperands()) * sizeof(uint32_t));
        setOutOfLineWords(copy);
    }

    void moveWordsFrom(Instruction& other)
    {
        copyLayoutFrom(other);
        m_words = other.m_words;
        other.m_hasOutOfLineWords = false;
    }

    void freeWords()
    {
        if (m_hasOutOfLineWords)
            fastFree(outOfLineWords().storage);
    }

public:
    void appendWordsTo(BodyWords& stream) const
    {
        if (!m_hasOutOfLineWords)
            return;
        OutOfLineWords words = outOfLineWords();
        stream.append(std::span<const uint32_t> { words.storage, outOfLineHeaderSize + static_cast<size_t>(words.numberOfPayloadWords) + words.numberOfOperands() });
    }
    // This is a copy, byte for byte, of an instruction that was written with appendWordsTo().
    void takeWordsFrom(BodyWordsReader& reader)
    {
        if (!m_hasOutOfLineWords)
            return;
        OutOfLineWords former = outOfLineWords();
        uint32_t numberOfOperands = reader.peek(1);
        size_t numberOfWordsBehindHeader = static_cast<size_t>(former.numberOfPayloadWords) + numberOfOperands;
        OutOfLineWords words = allocateWords(former.numberOfNamedOperands, numberOfOperands, numberOfOperands, former.numberOfPayloadWords);
        memcpy(words.payload(), reader.take(outOfLineHeaderSize + numberOfWordsBehindHeader).data() + outOfLineHeaderSize, numberOfWordsBehindHeader * sizeof(uint32_t));
        setOutOfLineWords(words);
    }
};

struct Value : Instruction {
    Representation representation { Representation::Unchosen };
    Opcode opcode { Opcode::Undefined };
    TypeID type;
private:
    TypeID m_expected;

public:
    Value() { resetOperandsAndPayload(numberOfNamedOperandsOf(Opcode::Undefined), defaultPayloadOf(Opcode::Undefined)); }

    explicit Value(Opcode newOpcode) { become(newOpcode); }

    void become(Opcode newOpcode)
    {
        opcode = newOpcode;
        resetOperandsAndPayload(numberOfNamedOperandsOf(newOpcode), defaultPayloadOf(newOpcode));
    }

    ExpectedType expected() const { return isExpectedNever() ? ExpectedType() : ExpectedType(m_expected); }
    void setExpected(ExpectedType expected) { m_expected = expected.predicted(); }
    ExpectedType expectedOrType() const { return ExpectedType(m_expected && !isExpectedNever() ? m_expected : type); }

    bool isExpectedNever() const { return !m_expected.index(); }
    ExpectedType expectedForTyping() const { return ExpectedType(m_expected); }
    ExpectedType expectedOrTypeForTyping() const { return ExpectedType(m_expected ? m_expected : type); }

    OperandKind operandKind(uint32_t operandIndex) const { return operandKindOf(opcode, operandIndex); }
    OperandUses operandUses(uint32_t operandIndex) const { return operandUsesOf(opcode, operandIndex); }

#include "AOTSchemaValueAccessors.inc"

    // The type that a cast names, whatever makes it true.
    TypeID typeNamedByCast() const
    {
        if (opcode == Opcode::Upcast)
            return castType();
        return opcode == Opcode::AssumeType ? provenType() : expectedType().predicted();
    }

    void becomeAssumeType(ValueID operand, ProvenType proven)
    {
        become(Opcode::AssumeType);
        setValueOperand(operand);
        setProvenType(proven);
        setIsBehindChecks(proven.scope() == ProofScope::BehindChecks);
    }
};

static_assert(sizeof(Value) <= 40);

// The value goes on as it is, and nothing more is known of it behind the cast.
inline constexpr OpcodeSet castsThatProveNothing { Opcode::Upcast, Opcode::Expect };
constexpr bool isCastThatProvesNothing(Opcode opcode) { return castsThatProveNothing.contains(opcode); }

template<> inline constexpr bool canMoveWithMemcpy<Value> = true;

} } } // namespace JSC::AOT::Typed

namespace WTF {

template<> struct VectorTraits<JSC::AOT::Typed::Value> : VectorTraitsBase<false, JSC::AOT::Typed::Value> {
    static constexpr bool canMoveWithMemcpy = true;
};

} // namespace WTF

#endif // ENABLE(AOT)
