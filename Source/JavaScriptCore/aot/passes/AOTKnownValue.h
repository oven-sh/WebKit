/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include <algorithm>
#include <array>
#include <bit>
#include <optional>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

class StringArena;

enum class KnownBoolean : uint8_t { Unknown, False, True };

class KnownValue {
public:
    enum class Kind : uint8_t { Unknown, Undefined, Null, Boolean, Integer, Double, String, Object, Function };

    KnownValue() = default;

    static KnownValue undefined() { return { Kind::Undefined, 0 }; }
    static KnownValue null() { return { Kind::Null, 0 }; }
    static KnownValue boolean(bool value) { return { Kind::Boolean, value }; }
    static KnownValue integer(int64_t value)
    {
        if (!Range::full().contains(value))
            return number(static_cast<double>(value));
        return { Kind::Integer, static_cast<uint64_t>(value) };
    }
    static KnownValue number(double);
    static KnownValue string(StringID value) { return { Kind::String, value.index() }; }
    static KnownValue object() { return { Kind::Object, 0 }; }
    static KnownValue function() { return { Kind::Function, 0 }; }

    Kind kind() const { return m_kind; }
    bool isKnown() const { return m_kind != Kind::Unknown; }
    bool isNumber() const { return m_kind == Kind::Integer || m_kind == Kind::Double; }
    bool isNullish() const { return m_kind == Kind::Undefined || m_kind == Kind::Null; }
    bool isObject() const { return m_kind == Kind::Object || m_kind == Kind::Function; }

    bool asBoolean() const
    {
        RELEASE_ASSERT(m_kind == Kind::Boolean);
        return m_bits;
    }

    int64_t asInteger() const
    {
        RELEASE_ASSERT(m_kind == Kind::Integer);
        return static_cast<int64_t>(m_bits);
    }

    double asNumber() const
    {
        if (m_kind == Kind::Integer)
            return static_cast<double>(static_cast<int64_t>(m_bits));
        RELEASE_ASSERT(m_kind == Kind::Double);
        return std::bit_cast<double>(m_bits);
    }

    StringID asString() const
    {
        RELEASE_ASSERT(m_kind == Kind::String);
        return StringID(static_cast<uint32_t>(m_bits));
    }

    KnownBoolean toBoolean(const Program&) const;

    friend bool operator==(const KnownValue&, const KnownValue&) = default;

private:
    KnownValue(Kind kind, uint64_t bits)
        : m_kind(kind)
        , m_bits(bits)
    {
    }

    Kind m_kind { Kind::Unknown };
    uint64_t m_bits { 0 };
};

constexpr KnownBoolean knownBoolean(bool value) { return value ? KnownBoolean::True : KnownBoolean::False; }

KnownValue knownValueOf(const Program&, const Function&, ValueID);
KnownValue knownValueOfType(const Program&, const Type&);
// Unknown for a hole, a bigint, a list and an object.
KnownValue knownValueOf(const DataNode&);
// The type that has the primitive and nothing else.
Type typeOfKnownPrimitive(KnownValue);
// The value keeps its number and its place and is the primitive from now on.
void becomeConstant(const Program&, Function&, TypeArena&, ValueID, KnownValue);

KnownBoolean evaluateTypeTest(const Program&, const Type& source, const Type& tested);
KnownBoolean evaluateTypeTest(const Program&, KnownValue, const Type& tested);
KnownBoolean evaluateIsClosureOf(const Program&, const Type& source, FunctionID);
KnownBoolean evaluateTestByTypes(const Program&, const Function&, const Value& test);
// The operation that defines the value never gives the candidate, although the type of the value has it.
bool isRuledOutByDefinition(const Program&, const Function&, ValueID, KnownValue candidate);

std::optional<int64_t> switchCaseOf(KnownValue);

inline constexpr OpcodeSet opcodesWithKnownValueRules {
    Opcode::Undefined, Opcode::Null, Opcode::Boolean, Opcode::Integer, Opcode::Double, Opcode::String,
    Opcode::Upcast, Opcode::NarrowByField, Opcode::AssumeType, Opcode::Check, Opcode::CheckInitialized, Opcode::RequireObjectCoercible, Opcode::RequireObject,
    Opcode::ToBoolean, Opcode::LogicalNot, Opcode::TypeTest,
    Opcode::StrictEq, Opcode::Eq, Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq,
    Opcode::Add, Opcode::Sub, Opcode::BitAnd, Opcode::ToNumber, Opcode::ToNumeric, Opcode::GetStringLength,
};

KnownValue evaluateOperation(const Program&, const Function&, const Value&, std::array<KnownValue, 2> operands);

KnownValue evaluateWithKnownOperands(const Program& program, const Function& function, const Value& value, NOESCAPE const Invocable<KnownValue(ValueID)> auto& operandValue)
{
    if (!opcodesWithKnownValueRules.contains(value.opcode))
        return { };
    std::array<KnownValue, 2> operands;
    for (uint32_t i = 0; i < std::min<uint32_t>(value.numberOfOperands(), 2); ++i) {
        if (ValueID operand = value.operand(i))
            operands[i] = operandValue(operand);
    }
    return evaluateOperation(program, function, value, operands);
}

inline constexpr OpcodeSet opcodesCreatingKnownStrings { Opcode::Add, Opcode::Strcat, Opcode::ToString };

KnownValue evaluateStringOperation(const Program&, StringArena&, const Function&, const Value&, NOESCAPE const ScopedLambda<KnownValue(ValueID)>& operandValue);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
