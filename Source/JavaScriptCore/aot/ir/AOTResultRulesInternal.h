/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBlock.h"
#include "AOTProvisionalFunctions.h"
#include "AOTResultRules.h"
#include "AOTSchemaResultRules.h"
#include "AOTStringTable.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

class WordHasher {
public:
    void add(uint32_t first, uint32_t second = 0)
    {
        m_hash = (m_hash ^ (static_cast<uint64_t>(first) << 32 | second)) * 0x9e3779b97f4a7c15ull;
        m_hash ^= m_hash >> 29;
    }

    void addNonIDWords(uint32_t first, uint32_t second = 0) { add(first, second); }

    void add(ValueID id) { add(id.index()); }
    void add(BlockID id) { add(id.index()); }
    void add(TypeID id) { add(id.index()); }

    void add(std::span<const uint32_t> words)
    {
        size_t index = 0;
        for (; index + 1 < words.size(); index += 2)
            add(words[index], words[index + 1]);
        if (index < words.size())
            add(words[index]);
    }

    void addPayloadOf(const Instruction& instruction)
    {
        std::array<uint32_t, numberOfInlinePayloadWords> inlineWords = instruction.inlinePayload();
        static_assert(numberOfInlinePayloadWords == 3);
        std::span<const uint32_t> otherWords = instruction.outOfLinePayload();
        add(instruction.flags | static_cast<uint32_t>(otherWords.size()) << 16, inlineWords[0]);
        add(inlineWords[1], inlineWords[2]);
        add(otherWords);
    }

    void addForEachOperandOf(const Instruction& instruction, NOESCAPE const Invocable<uint32_t(ValueID)> auto& wordOf)
    {
        uint32_t numberOfOperands = instruction.numberOfOperands();
        uint32_t index = 0;
        for (; index + 1 < numberOfOperands; index += 2)
            add(wordOf(instruction.operand(index)), wordOf(instruction.operand(index + 1)));
        if (index < numberOfOperands)
            add(wordOf(instruction.operand(index)));
    }

    uint64_t hash() const { return m_hash; }

private:
    uint64_t m_hash { 0x243f6a8885a308d3ull };
};

class SecondWordHasher {
public:
    void add(uint32_t first, uint32_t second = 0)
    {
        m_wordsPlusOne |= (first + 1) | (second + 1);
        addNonIDWords(first, second);
    }

    void addNonIDWords(uint32_t first, uint32_t second = 0)
    {
        m_hash = (m_hash ^ (static_cast<uint64_t>(second) << 32 | first)) * 0xc2b2ae3d27d4eb4full;
        m_hash ^= m_hash >> 31;
    }

    void add(TypeID id) { add(id.index()); }

    void add(std::span<const uint32_t> words)
    {
        size_t index = 0;
        for (; index + 1 < words.size(); index += 2)
            add(words[index], words[index + 1]);
        if (index < words.size())
            add(words[index]);
    }

    void addPayloadOf(const Instruction& instruction)
    {
        std::array<uint32_t, numberOfInlinePayloadWords> inlineWords = instruction.inlinePayload();
        static_assert(numberOfInlinePayloadWords == 3);
        std::span<const uint32_t> otherWords = instruction.outOfLinePayload();
        add(instruction.flags | static_cast<uint32_t>(otherWords.size()) << 16, inlineWords[0]);
        add(inlineWords[1], inlineWords[2]);
        add(otherWords);
    }

    void addForEachOperandOf(const Instruction& instruction, NOESCAPE const Invocable<uint32_t(ValueID)> auto& wordOf)
    {
        uint32_t numberOfOperands = instruction.numberOfOperands();
        uint32_t index = 0;
        for (; index + 1 < numberOfOperands; index += 2)
            add(wordOf(instruction.operand(index)), wordOf(instruction.operand(index + 1)));
        if (index < numberOfOperands)
            add(wordOf(instruction.operand(index)));
    }

    std::optional<SharedTypeMemo::Key> sharedKey(uint64_t firstHash) const
    {
        if (m_wordsPlusOne & highestBit)
            return std::nullopt;
        return SharedTypeMemo::keyOf(firstHash, m_hash);
    }

private:
    static constexpr uint32_t highestBit = 1u << 31;
    static_assert(TypeTable::provisionalBit == highestBit && StringTable::provisionalBit == highestBit && ProvisionalFunctions::firstIndex == highestBit);
    static_assert(!(TypeID().index() + 1));

    uint64_t m_hash { 0x13198a2e03707344ull };
    uint32_t m_wordsPlusOne { 0 };
};

TypeID sharedOrComputedType(const TypingContext& context, std::optional<SharedAnswers> answers, uint64_t hash, NOESCAPE const auto& addWords, NOESCAPE const Invocable<TypeID()> auto& compute)
{
    if (!answers || context.knownPositions || !context.types.table().sharedMemo(*answers))
        return compute();
    SecondWordHasher secondHasher;
    addWords(secondHasher);
    return context.types.sharedAnswer(*answers, secondHasher.sharedKey(hash), compute);
}

TypeID rememberedType(const TypingContext& context, std::optional<SharedAnswers> answers, NOESCAPE const auto& addWords, NOESCAPE const Invocable<TypeID()> auto& compute)
{
    WordHasher hasher;
    addWords(hasher);
    if (TypeID known = context.memo->results.find(hasher.hash()))
        return known;
    TypeID type = sharedOrComputedType(context, answers, hasher.hash(), addWords, compute);
    context.memo->results.add(hasher.hash(), type);
    return type;
}

inline constexpr OpcodeSet opcodesWithNaturalTypeRefinedByValue = Value::opcodesWithForm | OpcodeSet {
    Opcode::SharedRegExpLiteral, Opcode::GetTypedArrayElement, Opcode::DivideIntegers, Opcode::TypeTest, Opcode::ToInt32, Opcode::ToUint32,
    Opcode::ToIntegerOrInfinity, Opcode::ToLength, Opcode::LogicalNot, Opcode::ToObject, Opcode::ToThis
};

Type naturalTypeFrom(const NaturalType&);
Type naturalTypeOfValue(const TypingContext&, const Value&, NaturalTable);
Type refineFixedNatural(const TypingContext&, const Value&, const Type& upperBound);

std::optional<Type> tryTypeOfFieldRead(const TypingContext&, std::span<const LayoutID> receivers, PropertyKeyID);
std::optional<Type> tryTypeOfFieldRead(const TypingContext&, const Type& receiver, PropertyKeyID);
std::optional<Type> tryTypeOfSlotContents(const TypingContext&, std::span<const LayoutID> receivers, PropertyKeyID);
std::optional<Type> tryTypeOfSlotContents(const TypingContext&, LayoutSetID receivers, PropertyKeyID);
std::optional<Type> tryTypeOfSetterInSlot(const TypingContext&, std::span<const LayoutID> receivers, PropertyKeyID);
std::optional<Type> tryTypeOfSetterInSlot(const TypingContext&, LayoutSetID receivers, PropertyKeyID);
std::optional<Type> tryTypeOfDeferringSlotContents(const TypingContext&, std::span<const LayoutID> receivers, PropertyKeyID);
std::optional<Type> tryTypeOfDeferringSlotContents(const TypingContext&, LayoutSetID receivers, PropertyKeyID);
std::optional<Type> tryTypeOfContentsRead(const TypingContext&, const Type& container, ContentsSlot);
std::optional<Type> tryTypeOfElementRead(const TypingContext&, const Type& container, const Type& key);
std::optional<Type> tryTypeOfAwaited(const TypingContext&, const Type& awaited);

struct ResultRules {
    using Result = Type;

    const TypingContext& context;
    bool wasUnproven { false };

    Type none(const Instruction&);
    Type constant(const Value&);
    Type natural(const Value&, const NaturalType&);
    Type natural(const Terminator&, const NaturalType& type) { return naturalTypeFrom(type); }
    Type natural(const Value&, NaturalTable);
    Type sameAs(const Value&, uint32_t operandIndex);
    Type sameAs(const Terminator& terminator, uint32_t operandIndex) { return context.typeOf(terminator.operand(operandIndex)); }
    Type allocation(const Value&, LayoutID);
    Type allocation(const Value&, BuiltinClassID);
    Type allocation(const Value&, BuiltinClassID, TypeID contents);
    Type allocation(const Value&, BuiltinLayoutID);
    Type declared(const Value&, VariableID);
    Type declared(const Value&, LayoutID, uint32_t field);
    Type declared(const Value&, PropertyKeyID);
    Type declared(const Value&, FunctionID, uint32_t resultIndex);
    Type declared(const Value&, LayoutID);
    Type declared(const Value&, ModuleID);
    Type declared(const Value&, TypeID);
    Type declared(const Value&, BuiltinOverloadID, uint32_t resultIndex);
    Type declared(const Terminator&, BuiltinOverloadID, uint32_t resultIndex);
    Type declaredContents(const Value&, uint32_t operandIndex, std::optional<ContentsSlot>);
    Type declaredResult(const Value&, uint32_t operandIndex, uint32_t index);
    Type declaredParameter(const Value&);
    Type unknown(const Instruction&);
    Type unknown(const Value&);
    Type cast(const Value&, uint32_t operandIndex, TypeID castType);
    Type cast(const Value&, uint32_t operandIndex, ExpectedType);
    Type cast(const Value&, uint32_t operandIndex);
    Type resultOperand(const Value&, uint32_t resultIndex);

private:
    Type provenOrUnknown(std::optional<Type>&&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
