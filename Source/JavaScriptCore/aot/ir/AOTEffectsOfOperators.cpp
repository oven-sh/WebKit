/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEffectsDescription.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTIR.h"

namespace JSC { namespace AOT { namespace Typed {

static bool mayBeString(const Type& type)
{
    return type.primitives & Primitive::AnyString;
}

bool hasTextByItself(const Type& type)
{
    return !type.isNever() && type.hasOnlyPrimitives(Primitive::NumberLike | Primitive::AnyString | Primitive::BigInt);
}

namespace {

struct OperandClasses {
    bool convertsObjects { false };
    bool areAllInert { true };
    bool mayAllBeStrings { true };
    bool hasProtocol { false };
    bool haveAllTextByThemselves { true };
    bool hasString { false };

    void add(const Type& type, const OperandBehavior& behavior)
    {
        bool runsProtocol = !behavior.protocols.isEmpty();
        convertsObjects |= runsProtocol && type.hasObjects();
        areAllInert &= runsProtocol ? type.isNumberLike() : type.withoutObjects().isNumberLike();
        mayAllBeStrings &= mayBeString(type);
        hasProtocol |= runsProtocol;
        haveAllTextByThemselves &= hasTextByItself(type);
        hasString |= type.isAnyString();
    }

    // `text + 1`, String(1): the texts of primitives are joined. A number added to a BigInt is an error of the language.
    bool onlyMakesText(Opcode opcode) const
    {
        return haveAllTextByThemselves && (opcode == Opcode::ToString || (opcode == Opcode::Add && hasString));
    }
};

} // anonymous namespace

template<typename OperandTypeWalk>
static void describeOperatorOn(const Program& program, Opcode opcode, const Type* result, const Effects& declared, const OperandTypeWalk& forEachOperandType, EffectDescription& description)
{
    RELEASE_ASSERT(!declared.mayCallUnknownCode);

    OperandClasses classes;
    forEachOperandType([&](const Type& type, uint32_t operandIndex) {
        classes.add(type, declaredBehaviorOf(opcode, operandIndex));
    });

    if (classes.areAllInert && result && !result->hasCells())
        return;
    if (provesOf(opcode) == Proof::Equality && !classes.hasProtocol && !classes.mayAllBeStrings)
        return;

    Effects own = declared;
    // `a === b` converts nothing: all that it does to two texts is to flatten them.
    bool onlyComparesTexts = provesOf(opcode) == Proof::Equality && !classes.hasProtocol;
    if (own.mayThrow && (classes.onlyMakesText(opcode) || onlyComparesTexts))
        own.mayThrow = ThrowEffect::onlyForLackOfMemory();
    description.addOwn(own);
    if (!classes.convertsObjects)
        return;
    forEachOperandType([&](const Type& type, uint32_t operandIndex) {
        describeOperandCode(program, type, declaredBehaviorOf(opcode, operandIndex), description);
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
