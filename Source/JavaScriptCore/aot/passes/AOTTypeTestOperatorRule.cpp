/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeTestSite.h"

#if ENABLE(AOT)

#include "AOTOperatorForms.h"
#include "AOTPropertyAccessElements.h"
#include <array>

namespace JSC { namespace AOT { namespace Typed {

using TestedOperands = std::array<bool, 2>;
using TestedTypePerOperand = std::array<TypeID, 2>;

namespace {

struct OperatorTests {
    TestedOperands isTested { };
    Effects effectsWhenPassed;
};

} // anonymous namespace

static constexpr uint32_t leftOperand = 0;
static constexpr uint32_t rightOperand = 1;

static constexpr std::array<TestedOperands, 3> testedOperandSets { { { true, false }, { false, true }, { true, true } } };

static TypeID testedTypeContaining(const Type& expected)
{
    if (expected.isNumber())
        return testedNumberTypeContaining(expected);
    if (expected.isAnyString())
        return TypeTable::string;
    if (expected.isBoolean())
        return TypeTable::boolean;
    if (expected.isObject())
        return TypeTable::object;
    return { };
}

// Whoever asks whether a value is truthy reckons with undefined and null: `if (props.bold)` with `bold?: boolean`.
static TypeID admitExpectedNullish(TestSiteReporter& reporter, ValueID operand, TypeID tested)
{
    TypeArena& types = reporter.types();
    if (!(types.at(reporter.function().at(operand).expectedOrType()).primitives & Primitive::UndefinedOrNull))
        return tested;
    return types.intern(types.at(tested).withPrimitives(Primitive::UndefinedOrNull));
}

static TypeID testedTypeFor(TestSiteReporter& reporter, const Value& user, ValueID operand)
{
    std::optional<Type> expected = reporter.expectedPartOf(operand);
    if (!expected)
        return { };
    TypeID tested = testedTypeContaining(*expected);
    if (tested && (user.opcode == Opcode::ToBoolean || (user.opcode == Opcode::StrictEq && tested == TypeTable::object)))
        tested = admitExpectedNullish(reporter, operand, tested);
    if (!tested || reporter.typeOf(operand).isSubsetOf(reporter.types().at(tested), reporter.program()))
        return { };
    return tested;
}

static bool isNumberByTypeOrExpectedType(TestSiteReporter& reporter, ValueID operand)
{
    // An element of an array of numbers is a number or missing.
    if (reporter.typeOf(operand).isNumberOrNullish())
        return true;
    std::optional<Type> expected = reporter.expectedPartOf(operand);
    return expected && expected->isNumber();
}

// An expected type that names no primitive, or leaves out no kind of them, says nothing to an operator. Such an operand is taken for a number where the operand beside it is one.
static TypeID testedTypeBesideNumber(TestSiteReporter& reporter, ValueID operand, ValueID other)
{
    const Type& type = reporter.typeOf(operand);
    if (!(type.primitives & Primitive::Number) || type.isNumber() || !isNumberByTypeOrExpectedType(reporter, other))
        return { };
    constexpr uint16_t noPrimitives = 0;
    std::optional<Type> expected = reporter.expectedPartOf(operand, noPrimitives);
    if (expected && expected->primitives && expected->primitives != type.primitives)
        return { };
    return TypeTable::number;
}

static bool isBetterForm(Opcode opcode, const OperatorChoice& now, const OperatorChoice& then)
{
    if (then.form == OperatorForm::Generic || then.form == now.form)
        return false;
    if (now.form == OperatorForm::Generic)
        return true;
    if (now.effects.normal.mayCallUnknownCode && !then.effects.normal.mayCallUnknownCode)
        return true;
    if (mayCallRuntime(opcode, now.form) && !mayCallRuntime(opcode, then.form))
        return true;
    return now.form == OperatorForm::Numbers && (then.form == OperatorForm::Integers || then.form == OperatorForm::CheckedIntegers);
}

static OperatorChoice chooseFormUnderTests(TestSiteReporter& reporter, const OperatorSite& site, const Value& value, const TestedTypePerOperand& tested, const TestedOperands& isTested)
{
    TypeArena& types = reporter.types();
    Vector<Type, 2> operandTypes;
    for (uint32_t operandIndex = 0; operandIndex < value.numberOfOperands(); ++operandIndex) {
        const Type& type = reporter.typeOf(value.operand(operandIndex));
        operandTypes.append(isTested[operandIndex] ? type.intersectWith(types.at(tested[operandIndex]), types) : type);
    }
    return chooseOperatorFormFor(site, value.opcode, operandTypes.span());
}

// A later set replaces an earlier one only for a better form, so the operator is not reported again once its tests have passed.
static std::optional<OperatorTests> findFewestTests(TestSiteReporter& reporter, const OperatorSite& site, const Value& value, const TestedTypePerOperand& tested)
{
    std::optional<TestedOperands> chosen;
    OperatorChoice best = chooseOperatorForm(site, value);
    for (const TestedOperands& isTested : testedOperandSets) {
        if ((isTested[leftOperand] && !tested[leftOperand]) || (isTested[rightOperand] && !tested[rightOperand]))
            continue;
        OperatorChoice then = chooseFormUnderTests(reporter, site, value, tested, isTested);
        if (!isBetterForm(value.opcode, best, then))
            continue;
        best = WTF::move(then);
        chosen = isTested;
    }
    if (!chosen)
        return std::nullopt;
    return OperatorTests { *chosen, best.effects.normal };
}

// The one rule behind this one looks at tests of a type, so a site that is left out here is claimed from nobody.
static_assert(!hasOperatorForm(Opcode::TypeTest));

void reportOperatorTests(TestSiteReporter& reporter)
{
    TypingContext typing { reporter.program(), reporter.function(), reporter.types() };
    OperatorSite site { typing, reporter.analyses().effectContext(), CodeShape::Fast };

    reporter.forEachCandidate([&](const Value& value) {
        if (!hasOperatorForm(value.opcode) || reporter.takesAwaySitesInColdCode())
            return;
        uint32_t numberOfOperands = value.numberOfOperands();
        RELEASE_ASSERT(numberOfOperands == 1 || numberOfOperands == 2);

        TestedTypePerOperand tested;
        for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex)
            tested[operandIndex] = testedTypeFor(reporter, value, value.operand(operandIndex));
        if (numberOfOperands == 2) {
            for (uint32_t operandIndex : { leftOperand, rightOperand }) {
                if (tested[operandIndex] && tested[operandIndex] != TypeTable::object)
                    continue;
                if (TypeID number = testedTypeBesideNumber(reporter, value.operand(operandIndex), value.operand(1 - operandIndex)))
                    tested[operandIndex] = number;
            }
        }
        if (!tested[leftOperand] && !tested[rightOperand])
            return;

        std::optional<OperatorTests> found = findFewestTests(reporter, site, value, tested);
        if (!found)
            return;

        Vector<Value, 2> tests;
        for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
            bool repeatsLeftTest = operandIndex == rightOperand && found->isTested[leftOperand]
                && value.operand(leftOperand) == value.operand(rightOperand) && tested[leftOperand] == tested[rightOperand];
            if (found->isTested[operandIndex] && !repeatsLeftTest)
                tests.append(makeTypeTest(value.operand(operandIndex), tested[operandIndex]));
        }
        reporter.report(tests.span(), found->effectsWhenPassed);
    });
}

void rewriteTestedOperator(TestSiteRewriter&)
{
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
