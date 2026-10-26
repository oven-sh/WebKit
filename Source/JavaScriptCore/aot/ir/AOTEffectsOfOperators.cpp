/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEffectsDescription.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTIR.h"
#include "JSString.h"
#include <wtf/dtoa.h>

namespace JSC { namespace AOT { namespace Typed {

static bool mayBeString(const Type& type)
{
    return type.primitives & Primitive::AnyString;
}

bool hasTextByItself(const Type& type)
{
    return !type.isNever() && type.hasOnlyPrimitives(Primitive::NumberLike | Primitive::AnyString | Primitive::BigInt);
}

// A string of the program is held in no fewer bytes than it has characters.
static uint64_t longestTextOf(const Program& program, const Type& type)
{
    constexpr uint64_t longestTextOfNumberLike = std::tuple_size_v<NumberToStringBuffer>;
    static_assert(longestTextOfNumberLike >= "undefined"_s.length());

    if (type.primitives & (Primitive::String | Primitive::BigInt))
        return JSString::MaxLength;
    uint64_t longest = type.primitives & Primitive::NumberLike ? longestTextOfNumberLike : 0;
    for (StringID literal : type.stringLiterals)
        longest = std::max<uint64_t>(longest, program.strings[literal].size());
    return longest;
}

void JoinedTexts::add(const Program& program, const Type& type)
{
    m_haveAllTextByThemselves &= hasTextByItself(type);
    if (m_haveAllTextByThemselves)
        m_longest += longestTextOf(program, type);
}

bool JoinedTexts::fitInString() const
{
    return m_haveAllTextByThemselves && m_longest <= JSString::MaxLength;
}

// IsLooselyEqual converts an object only to compare it with a boolean, number, string, symbol or BigInt.
static bool isComparedAsPrimitive(const Type& otherOperand)
{
    return otherOperand.primitives & ~Primitive::UndefinedOrNull;
}

namespace {

struct OperandClasses {
    bool convertsObjects { false };
    bool areAllInert { true };
    bool mayAllBeStrings { true };
    bool hasProtocol { false };
    bool hasString { false };
    JoinedTexts texts;

    void add(const Program& program, const Type& type, const OperandBehavior& behavior)
    {
        bool runsProtocol = !behavior.protocols.isEmpty();
        convertsObjects |= runsProtocol && type.hasObjects();
        areAllInert &= runsProtocol ? type.isNumberLike() : type.withoutObjects().isNumberLike();
        mayAllBeStrings &= mayBeString(type);
        hasProtocol |= runsProtocol;
        hasString |= type.isAnyString();
        texts.add(program, type);
    }

    bool makesTextThatFitsInString(Opcode opcode) const
    {
        return texts.fitInString() && (opcode == Opcode::ToString || (opcode == Opcode::Add && hasString));
    }
};

} // anonymous namespace

template<typename OperandTypeWalk>
static void describeOperatorOn(const Program& program, Opcode opcode, const Type* result, const Effects& declared, const OperandTypeWalk& forEachOperandType, EffectDescription& description)
{
    RELEASE_ASSERT(!declared.mayCallUnknownCode);

    Vector<const Type*, 2> operandTypes;
    forEachOperandType([&](const Type& type, uint32_t) {
        operandTypes.append(&type);
    });
    auto behaviorOf = [&](uint32_t operandIndex) {
        OperandBehavior behavior = declaredBehaviorOf(opcode, operandIndex);
        if (opcode == Opcode::Eq && !isComparedAsPrimitive(*operandTypes[1 - operandIndex]))
            behavior.protocols = { };
        return behavior;
    };

    OperandClasses classes;
    forEachOperandType([&](const Type& type, uint32_t operandIndex) {
        classes.add(program, type, behaviorOf(operandIndex));
    });

    if (classes.areAllInert && result && !result->hasCells())
        return;
    if (provesOf(opcode) == Proof::Equality && !classes.hasProtocol && !classes.mayAllBeStrings)
        return;

    Effects own = declared;
    bool onlyComparesTexts = provesOf(opcode) == Proof::Equality && !classes.hasProtocol;
    if (own.mayThrow && (classes.makesTextThatFitsInString(opcode) || onlyComparesTexts))
        own.mayThrow = ThrowEffect::onlyForLackOfMemory();
    description.addOwn(own);
    if (!classes.convertsObjects)
        return;
    forEachOperandType([&](const Type& type, uint32_t operandIndex) {
        describeOperandCode(program, type, behaviorOf(operandIndex), description);
    });
}

void describeOperator(const Program& program, const Function& function, const Value& value, const Effects& declared, EffectDescription& description)
{
    const Type* result = value.type ? &program.at(value.type) : nullptr;
    describeOperatorOn(program, value.opcode, result, declared, [&](const auto& functor) {
        value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            functor(program.typeOf(function, operand), operandIndex);
        });
    }, description);
}

void describeOperator(const Program& program, Opcode opcode, std::span<const Type> operandTypes, const Type& result, EffectDescription& description)
{
    RELEASE_ASSERT(effectRuleOf(opcode) == EffectRule::Operator);
    describeOperatorOn(program, opcode, &result, declaredBoundOf(opcode), [&](const auto& functor) {
        for (uint32_t operandIndex = 0; operandIndex < operandTypes.size(); ++operandIndex)
            functor(operandTypes[operandIndex], operandIndex);
    }, description);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
