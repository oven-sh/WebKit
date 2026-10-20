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
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class Instruction {
public:
    static constexpr uint32_t noPosition = UINT32_MAX;

    Arguments arguments;
private:
    CompactList<uint32_t> m_outOfLinePayload;
public:
    std::array<ValueID, numberOfInlineOperands> children;
private:
    std::array<uint32_t, numberOfInlinePayloadWords> m_payload { absentPayloadWord, absentPayloadWord, absentPayloadWord };
    static_assert(numberOfInlinePayloadWords == 3);
public:
    uint32_t position { noPosition };
    uint32_t inlineFrame { 0 };
    uint16_t flags { 0 };
private:
    uint8_t m_numberOfNamedOperands { 0 };

public:
    ValueID child(unsigned index) const { return children[index]; }

    uint32_t numberOfNamedOperands() const { return m_numberOfNamedOperands; }
    uint32_t numberOfOperands() const { return inlineOperandCount() + static_cast<uint32_t>(arguments.size()); }
    uint32_t numberOfVariadicOperands() const { return static_cast<uint32_t>(arguments.size()) - namedOperandsInArguments(); }

    ValueID operand(uint32_t operandIndex) const
    {
        uint32_t inlineCount = inlineOperandCount();
        return operandIndex < inlineCount ? children[operandIndex] : arguments[operandIndex - inlineCount];
    }

    void setOperand(uint32_t operandIndex, ValueID id)
    {
        uint32_t inlineCount = inlineOperandCount();
        if (operandIndex < inlineCount)
            children[operandIndex] = id;
        else
            arguments[operandIndex - inlineCount] = id;
    }

    std::span<const ValueID> variadicOperands() const LIFETIME_BOUND { return arguments.subspan(namedOperandsInArguments()); }
    std::span<ValueID> mutableVariadicOperands() LIFETIME_BOUND { return arguments.mutableSpan().subspan(namedOperandsInArguments()); }
    void appendVariadicOperand(ValueID id) { arguments.append(id); }

    void setVariadicOperands(std::span<const ValueID> operands)
    {
        Vector<ValueID, 8> copy(operands);
        arguments.shrink(namedOperandsInArguments());
        arguments.append(copy.span());
    }

    void removeVariadicOperand(uint32_t variadicIndex) { arguments.removeAt(namedOperandsInArguments() + variadicIndex); }

    std::optional<uint32_t> variadicOperandIndex(uint32_t operandIndex) const
    {
        if (operandIndex < m_numberOfNamedOperands)
            return std::nullopt;
        return operandIndex - m_numberOfNamedOperands;
    }

    template<typename Functor>
    void forEachOperand(const Functor& functor) const { forEachOperandOf(*this, functor); }

    template<typename Functor>
    void forEachOperand(const Functor& functor) { forEachOperandOf(*this, functor); }

    template<typename Functor>
    void forEachChild(const Functor& functor) const
    {
        forEachOperandOf(*this, [&](ValueID operand, uint32_t) {
            functor(operand);
        });
    }

    template<typename Functor>
    void forEachChild(const Functor& functor)
    {
        forEachOperandOf(*this, [&](ValueID& operand, uint32_t) {
            functor(operand);
        });
    }

    bool hasPosition() const { return position != noPosition; }
    Origin origin() const { return { position, inlineFrame }; }
    void setOrigin(Origin origin)
    {
        position = origin.position;
        inlineFrame = origin.inlineFrame;
    }

    uint32_t payloadWordAt(unsigned word) const { return word < m_payload.size() ? m_payload[word] : m_outOfLinePayload[word - m_payload.size()]; }
    void setPayloadWordAt(unsigned word, uint32_t value) { mutablePayloadWordAt(word) = value; }
    uint64_t payloadWordPairAt(unsigned word) const { return static_cast<uint64_t>(payloadWordAt(word + 1)) << 32 | payloadWordAt(word); }
    void setPayloadWordPairAt(unsigned word, uint64_t value)
    {
        setPayloadWordAt(word, static_cast<uint32_t>(value));
        setPayloadWordAt(word + 1, static_cast<uint32_t>(value >> 32));
    }

    std::span<const uint32_t> payloadListFrom(unsigned firstWord) const LIFETIME_BOUND { return m_outOfLinePayload.subspan(listOffset(firstWord)); }
    std::span<uint32_t> mutablePayloadListFrom(unsigned firstWord) LIFETIME_BOUND { return m_outOfLinePayload.mutableSpan().subspan(listOffset(firstWord)); }
    void setPayloadListFrom(unsigned firstWord, std::span<const uint32_t> words)
    {
        Vector<uint32_t, 16> copy(words);
        m_outOfLinePayload.shrink(listOffset(firstWord));
        m_outOfLinePayload.append(copy.span());
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

    std::span<const uint32_t> inlinePayload() const LIFETIME_BOUND { return m_payload; }
    std::span<const uint32_t> outOfLinePayload() const LIFETIME_BOUND { return m_outOfLinePayload.span(); }

    bool hasSamePayloadAs(const Instruction& other) const { return flags == other.flags && m_payload == other.m_payload && m_outOfLinePayload == other.m_outOfLinePayload; }

    void addPayloadTo(Hasher& hasher) const
    {
        add(hasher, flags);
        for (uint32_t word : m_payload)
            add(hasher, word);
        add(hasher, static_cast<uint32_t>(m_outOfLinePayload.size()));
        for (uint32_t word : m_outOfLinePayload)
            add(hasher, word);
    }

protected:
    Instruction() = default;

    void resetOperandsAndPayload(uint32_t numberOfNamedOperands, std::span<const uint32_t> defaultPayload)
    {
        RELEASE_ASSERT(numberOfNamedOperands <= UINT8_MAX);
        m_numberOfNamedOperands = static_cast<uint8_t>(numberOfNamedOperands);
        flags = 0;
        children.fill(ValueID());
        arguments.clear();
        arguments.grow(namedOperandsInArguments());
        m_payload.fill(absentPayloadWord);
        size_t inlineCount = std::min(defaultPayload.size(), m_payload.size());
        std::copy_n(defaultPayload.begin(), inlineCount, m_payload.begin());
        m_outOfLinePayload = CompactList<uint32_t>(defaultPayload.subspan(inlineCount));
    }

private:
    uint32_t inlineOperandCount() const { return std::min<uint32_t>(m_numberOfNamedOperands, numberOfInlineOperands); }
    uint32_t namedOperandsInArguments() const { return m_numberOfNamedOperands - inlineOperandCount(); }

    template<typename InstructionType, typename Functor>
    static void forEachOperandOf(InstructionType& instruction, const Functor& functor)
    {
        uint32_t operandIndex = 0;
        for (uint32_t end = instruction.inlineOperandCount(); operandIndex < end; ++operandIndex) {
            if (auto& child = instruction.children[operandIndex])
                functor(child, operandIndex);
        }
        for (auto& argument : instruction.arguments) {
            if (argument)
                functor(argument, operandIndex);
            ++operandIndex;
        }
    }

    uint32_t& mutablePayloadWordAt(unsigned word) LIFETIME_BOUND { return word < m_payload.size() ? m_payload[word] : m_outOfLinePayload[word - m_payload.size()]; }

    size_t listOffset(unsigned firstWord) const
    {
        RELEASE_ASSERT(firstWord >= m_payload.size());
        size_t offset = firstWord - m_payload.size();
        RELEASE_ASSERT(offset <= m_outOfLinePayload.size());
        return offset;
    }
};

struct Value : Instruction {
    Representation representation { Representation::Unchosen };
    Opcode opcode { Opcode::Undefined };
    TypeID type;
private:
    TypeID m_written;
    TypeID m_prediction;

public:
    Value() { resetOperandsAndPayload(numberOfNamedOperandsOf(Opcode::Undefined), defaultPayloadOf(Opcode::Undefined)); }

    explicit Value(Opcode newOpcode) { become(newOpcode); }

    void become(Opcode newOpcode)
    {
        opcode = newOpcode;
        resetOperandsAndPayload(numberOfNamedOperandsOf(newOpcode), defaultPayloadOf(newOpcode));
    }

    TypeID written() const { return m_written; }
    void setWritten(TypeID written) { m_written = written; }
    TypeID writtenOrType() const { return m_written ? m_written : type; }

    TypeID prediction() const { return m_prediction; }
    void setPrediction(TypeID prediction) { m_prediction = prediction; }
    TypeID predictionOrType() const { return m_prediction ? m_prediction : writtenOrType(); }

    OperandKind operandKind(uint32_t operandIndex) const { return operandKindOf(opcode, operandIndex); }
    OperandUses operandUses(uint32_t operandIndex) const { return operandUsesOf(opcode, operandIndex); }

#include "AOTSchemaValueAccessors.inc"
};

static_assert(sizeof(Value) <= 72);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
