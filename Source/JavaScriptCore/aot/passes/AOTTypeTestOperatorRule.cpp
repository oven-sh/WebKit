/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeTestSite.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTBuiltinTypes.h"
#include "AOTInductionVariables.h"
#include "AOTInlineCopy.h"
#include "AOTInlineCostModel.h"
#include "AOTNaturalLoops.h"
#include "AOTOperatorForms.h"
#include "AOTPropertyAccessElements.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

using TestedOperands = std::array<bool, 2>;
using TestedTypePerOperand = std::array<TypeID, 2>;

namespace {

struct OperatorTests {
    TestedOperands isTested { };
    Effects effectsWhenPassed;
};

// A counter, and the comparison that keeps it on this side of a bound while the loop goes on.
struct BoundedCounter {
    ValueID parameter;
    ValueID comparison;
    Edge stayingEdge;
};

using BoundedCounters = Vector<BoundedCounter, 2>;

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

// Whoever asks whether a value is truthy, or whether it is the same as another, reckons with undefined and null: `if (props.bold)`, `kept !== props.bold` with `bold?: boolean`.
static constexpr OpcodeSet operatorsThatTakeNullish { Opcode::ToBoolean, Opcode::StrictEq, Opcode::Eq };

static TypeID admitExpectedNullish(TestSiteReporter& reporter, ValueID operand, TypeID tested)
{
    TypeArena& types = reporter.types();
    if (!(types.at(reporter.function().at(operand).expectedOrType()).primitives & Primitive::UndefinedOrNull))
        return tested;
    return types.intern(types.at(tested).withPrimitives(Primitive::UndefinedOrNull));
}

static constexpr OpcodeSet testedBitwiseOperators { Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::LShift, Opcode::RShift, Opcode::URShift, Opcode::BitNot };
static constexpr OpcodeSet numericOperators = testedBitwiseOperators | relationalOperators | OpcodeSet { Opcode::Increment, Opcode::Decrement, Opcode::Sub, Opcode::Mul, Opcode::Negate };

// A counter, for one, or a sum of integers. It is held as an integer, and of that a bitwise operator takes the low half, whatever its range. What comes out of memory or out of a call is a value, which the test is the cheapest way to look at.
static bool isHeldAsInteger(TestSiteReporter& reporter, const OperatorSite& site, ValueID operand)
{
    if (!reporter.typeOf(operand).isInteger())
        return false;
    ValueID identity = reporter.analyses().identityOf(operand);
    const Value& definition = reporter.function().at(identity);
    if (hasOperatorForm(definition.opcode) && chooseFormOfOperator(site, definition) == OperatorForm::Integers)
        return true;
    ValueLocation location = reporter.analyses().useLists().locationOf(identity);
    return location.placement == ValuePlacement::Parameter && location.block != entryBlock;
}

static TypeID testedTypeFor(TestSiteReporter& reporter, const OperatorSite& site, const Value& user, ValueID operand)
{
    if (testedBitwiseOperators.contains(user.opcode) && isHeldAsInteger(reporter, site, operand))
        return { };
    std::optional<Type> expected = reporter.expectedPartOf(operand);
    if (!expected)
        return { };
    TypeID tested = testedTypeContaining(*expected);
    if (tested && operatorsThatTakeNullish.contains(user.opcode))
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
static TypeID testedTypeBesideNumber(TestSiteReporter& reporter, const Value& user, ValueID operand, ValueID other)
{
    const Type& type = reporter.typeOf(operand);
    if (!(type.primitives & Primitive::Number) || type.isNumber() || !isNumberByTypeOrExpectedType(reporter, other))
        return { };
    constexpr uint16_t noPrimitives = 0;
    std::optional<Type> expected = reporter.expectedPartOf(operand, noPrimitives);
    if (expected && expected->primitives && expected->primitives != type.primitives)
        return { };
    // What is compared with a number that may be left out may have been left out too.
    return operatorsThatTakeNullish.contains(user.opcode) ? admitExpectedNullish(reporter, other, TypeTable::number) : TypeTable::number;
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
        if (isTested[operandIndex]) {
            operandTypes.append(type.intersectWith(types.at(tested[operandIndex]), types));
            continue;
        }
        // A number that only unlikely paths make other than an integer is one again once those paths have left: SeparateUnlikelyLoopPaths.
        std::optional<Type> expected = type.isNumber() ? reporter.expectedPartOf(value.operand(operandIndex)) : std::nullopt;
        operandTypes.append(expected && expected->isInteger() ? *expected : type);
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

static constexpr OpcodeSet operationsOnElementsOfViews { Opcode::GetTypedArrayElement, Opcode::PutTypedArrayElement, Opcode::IsInsideView };
static constexpr OpcodeSet operationsOnElements { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas };
static constexpr uint32_t collectionOperandIndex = 0;
static constexpr uint32_t positionOperandIndex = 1;
static constexpr size_t maxNamesOfCounter = 16;

static bool findsElementOfView(TestSiteReporter& reporter, const Use& use)
{
    const Value& user = reporter.function().at(use.user);
    if (use.operandIndex != positionOperandIndex)
        return false;
    if (operationsOnElementsOfViews.contains(user.opcode))
        return true;
    if (!operationsOnElements.contains(user.opcode))
        return false;
    const Type& collection = reporter.program().expectedOrTypeOf(reporter.function(), user.operand(collectionOperandIndex));
    return typedArrayTypeOf(BuiltinTable::singleton(), collection.withoutPrimitives(Primitive::UndefinedOrNull)) != NotTypedArray;
}

// Whether typed code in the loop does nothing with the counter but reckon, compare and find elements of views. A call, a store or a return wants a value, and to make one of an integer beyond int32 takes more code than the test does. An array counts its elements in 32 bits, so it wants an int32 too. Behind the loop the value is made once, and the test would be made at every turn.
static bool isOnlyReckonedWith(TestSiteReporter& reporter, const NaturalLoop& loop, ValueID counter)
{
    const Function& function = reporter.function();
    const UseLists& useLists = reporter.analyses().useLists();
    const NaturalLoops& loops = reporter.analyses().naturalLoops();
    Vector<ValueID, maxNamesOfCounter> names { counter };
    auto follow = [&](ValueID name) {
        if (names.contains(name))
            return true;
        if (names.size() == maxNamesOfCounter)
            return false;
        names.append(name);
        return true;
    };
    for (size_t index = 0; index < names.size(); ++index) {
        for (const Use& use : useLists.usesOf(names[index])) {
            if (function.at(use.block).frequency != BlockFrequency::Normal || !loops.belongsTo(use.block, loop))
                continue;
            switch (use.kind) {
            case Use::Kind::Operand: {
                Opcode opcode = function.at(use.user).opcode;
                if (isCastWithoutTest(opcode)) {
                    if (!follow(use.user))
                        return false;
                    continue;
                }
                if (opcode == Opcode::TypeTest || numericOperators.contains(opcode) || findsElementOfView(reporter, use))
                    continue;
                if (opcode == Opcode::Add && reporter.typeOf(use.user).isNumber())
                    continue;
                return false;
            }
            case Use::Kind::EdgeArgument: {
                BlockID targetBlock = targetOf(function, use.edge()).block;
                const Block& target = function.at(targetBlock);
                if (target.frequency == BlockFrequency::Normal && loops.belongsTo(targetBlock, loop) && (use.operandIndex >= target.parameters.size() || !follow(target.parameters[use.operandIndex])))
                    return false;
                continue;
            }
            case Use::Kind::TerminatorOperand:
            case Use::Kind::HandlerArgument:
            case Use::Kind::BlockSubject:
                return false;
            }
        }
    }
    return true;
}

// `i < bytes.length; i++`. On this side of a bound that is an integer the counter is one, which ConvertCountersToIntegers finds out behind the rounds. A bound for which a rule in front of this one has just asked for tests may be an integer behind them. Below an int32 a test for int32 does no harm: it is proven then, and goes.
static BoundedCounters findBoundedCounters(TestSiteReporter& reporter, std::span<const ValueID> comparisonsWithTestedBound)
{
    BoundedCounters counters;
    for (const LoopInduction& loop : reporter.analyses().inductionVariables().loops()) {
        for (const InductionVariable& variable : loop.variables) {
            if (variable.step.kind != InductionStepKind::Constant || !variable.start || !reporter.typeOf(variable.start).isInteger())
                continue;
            std::optional<bool> isEligible;
            for (const InductionTest& test : variable.tests) {
                if (test.tested != TestedValue::BeforeStep || !isBoundingRelation(test.relation, variable.direction))
                    continue;
                if (std::ranges::contains(comparisonsWithTestedBound, test.comparison)) {
                    counters.append({ variable.parameter, test.comparison, test.stayingEdge });
                    continue;
                }
                const Type& bound = reporter.typeOf(test.bound);
                if (!(bound.isInteger() && !bound.isInt32()) && !reporter.isClaimed(reporter.analyses().identityOf(test.bound)))
                    continue;
                if (!isEligible)
                    isEligible = isOnlyReckonedWith(reporter, reporter.analyses().naturalLoops().loop(loop.loopIndex), variable.parameter);
                if (*isEligible)
                    counters.append({ variable.parameter, test.comparison, test.stayingEdge });
            }
        }
    }
    return counters;
}

// An operator takes an integer as it is, so none of these needs a test. One for int32 that is placed here stays in the loop for good: a view may be longer than that, and nothing proves that this one is not.
static bool testsOnlyBoundedCounter(TestSiteReporter& reporter, const Value& value, const TestedOperands& isTested, std::optional<BoundedCounters>& boundedCounters, std::span<const ValueID> comparisonsWithTestedBound)
{
    // Behind the rounds the counters that could be converted have been.
    if (reporter.context().isBehindRounds || !reporter.analyses().blockCycles().innermostCycleOf(reporter.point().block))
        return false;
    if (!boundedCounters)
        boundedCounters = findBoundedCounters(reporter, comparisonsWithTestedBound);
    return std::ranges::any_of(*boundedCounters, [&](const BoundedCounter& counter) {
        for (uint32_t operandIndex = 0; operandIndex < value.numberOfOperands(); ++operandIndex) {
            if (isTested[operandIndex] && reporter.analyses().identityOf(value.operand(operandIndex)) != counter.parameter)
                return false;
        }
        return reporter.use() == counter.comparison || dominatesAfterSplit(reporter.analyses(), counter.stayingEdge, reporter.point().block);
    });
}

// What a function of the program does with an argument shows once the call is inlined.
static bool staysCall(TestSiteReporter& reporter, BlockID block, ValueID call)
{
    FunctionPassContext& context = reporter.context();
    const Value& value = reporter.function().at(call);
    if (value.isNeverInlined() || value.isAfterInlinedHead())
        return true;
    const InliningSummaries& inlining = context.inputs.inlining;
    if (!inlining.contains(value.function()))
        return false;
    const InliningSummary& summary = inlining.of(reporter.program(), value.function());
    if (canInline(context, summary, call) != InlineDecision::Unknown)
        return true;
    InlineDecision decision = computeInlineVerdict(context, summary, block, call).decision;
    return decision != InlineDecision::Inline && decision != InlineDecision::InlineHead;
}

// Whether a likely turn of the loop does something with the counter that an integer is better for: an operator gets a better form, or the counter finds an element or goes into a built-in function, or into a call: an integer stays in its register around the call and takes one instruction to become a value of any type. Whatever else takes it, a number that it is added to or a variable of numbers, takes it as a number for nothing.
static bool isWantedAsInteger(TestSiteReporter& reporter, const OperatorSite& site, const NaturalLoop& loop, const InductionVariable& counter)
{
    const Function& function = reporter.function();
    const NaturalLoops& loops = reporter.analyses().naturalLoops();
    Vector<ValueID, maxNamesOfCounter> names { counter.parameter };
    for (size_t index = 0; index < names.size(); ++index) {
        for (const Use& use : reporter.analyses().useLists().usesOf(names[index])) {
            if (use.kind != Use::Kind::Operand || function.at(use.block).frequency != BlockFrequency::Normal || !loops.belongsTo(use.block, loop) || counter.steppedValues.contains(use.user))
                continue;
            const Value& user = function.at(use.user);
            if (isCastWithoutTest(user.opcode)) {
                if (!names.contains(use.user) && names.size() < maxNamesOfCounter)
                    names.append(use.user);
                continue;
            }
            if (hasOperatorForm(user.opcode)) {
                TestedTypePerOperand tested;
                TestedOperands isTested { };
                tested[use.operandIndex] = TypeTable::int32;
                isTested[use.operandIndex] = true;
                if (isBetterForm(user.opcode, chooseOperatorForm(site, user), chooseFormUnderTests(reporter, site, user, tested, isTested)))
                    return true;
                continue;
            }
            OperandKind kind = user.operandKind(use.operandIndex);
            if (kind == OperandKind::Number || kind == OperandKind::Integer || user.opcode == Opcode::TypeTest || user.opcode == Opcode::CallBuiltin)
                return true;
            if (user.opcode == Opcode::Call || (user.opcode == Opcode::CallDirect && staysCall(reporter, use.block, use.user)))
                return true;
            if (operationsOnElements.contains(user.opcode) && use.operandIndex == positionOperandIndex)
                return true;
        }
    }
    return false;
}

// `i < n; i++` with `n: number`. Below an int32 the counter is one.
static std::optional<uint32_t> findNumberThatBoundsCounter(TestSiteReporter& reporter, const OperatorSite& site, const Value& comparison)
{
    if (!relationalOperators.contains(comparison.opcode))
        return std::nullopt;
    auto mayBeBound = [&](uint32_t operandIndex) {
        const Type& bound = reporter.typeOf(comparison.operand(operandIndex));
        return bound.isNumber() && !bound.isInteger() && reporter.typeOf(comparison.operand(1 - operandIndex)).isNumberWithoutFraction();
    };
    if (!mayBeBound(leftOperand) && !mayBeBound(rightOperand))
        return std::nullopt;
    const NaturalLoops& loops = reporter.analyses().naturalLoops();
    const NaturalLoop* loop = loops.innerMostLoopOf(reporter.point().block);
    if (!loop)
        return std::nullopt;
    // The test pays in front of the loop alone. Its proof has to last around the loop then, through code that the copy holds too, and a loop in there is more than a copy may hold.
    for (unsigned index = 0; index < loop->size(); ++index) {
        if (loops.innerMostLoopOf(loop->at(index)) != loop)
            return std::nullopt;
    }

    for (const InductionVariable& variable : reporter.analyses().inductionVariables().of(*loop).variables) {
        if (variable.step.kind != InductionStepKind::Constant || !variable.start || !reporter.typeOf(variable.start).isInt32())
            continue;
        for (const InductionTest& test : variable.tests) {
            if (test.comparison != reporter.use() || test.tested != TestedValue::BeforeStep || test.boundKind != LoopBoundKind::Invariant || !isBoundingRelation(test.relation, variable.direction))
                continue;
            ValueID bound = reporter.analyses().identityOf(test.bound);
            bool isLeftBound = reporter.analyses().identityOf(comparison.operand(leftOperand)) == bound;
            bool isRightBound = reporter.analyses().identityOf(comparison.operand(rightOperand)) == bound;
            if (isLeftBound == isRightBound || !mayBeBound(isLeftBound ? leftOperand : rightOperand) || !isWantedAsInteger(reporter, site, *loop, variable))
                return std::nullopt;
            return isLeftBound ? leftOperand : rightOperand;
        }
    }
    return std::nullopt;
}

// The one rule behind this one looks at tests of a type, so a site that is left out here is claimed from nobody.
static_assert(!hasOperatorForm(Opcode::TypeTest));

void reportOperatorTests(TestSiteReporter& reporter)
{
    TypingContext typing { reporter.program(), reporter.function(), reporter.types() };
    OperatorSite site { typing, reporter.analyses().effectContext(), CodeShape::Fast };

    std::optional<BoundedCounters> boundedCounters;
    Vector<ValueID, 2> comparisonsWithTestedBound;
    reporter.forEachCandidate([&](const Value& value) {
        if (!hasOperatorForm(value.opcode) || reporter.takesAwaySitesInColdCode())
            return;
        uint32_t numberOfOperands = value.numberOfOperands();
        RELEASE_ASSERT(numberOfOperands == 1 || numberOfOperands == 2);

        if (std::optional<uint32_t> bound = findNumberThatBoundsCounter(reporter, site, value)) {
            reporter.report({ makeTypeTest(value.operand(*bound), TypeTable::int32) }, chooseOperatorForm(site, value).effects.normal);
            comparisonsWithTestedBound.append(reporter.use());
            boundedCounters = std::nullopt;
            return;
        }

        TestedTypePerOperand tested;
        for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex)
            tested[operandIndex] = testedTypeFor(reporter, site, value, value.operand(operandIndex));
        if (numberOfOperands == 2) {
            for (uint32_t operandIndex : { leftOperand, rightOperand }) {
                if (tested[operandIndex] && tested[operandIndex] != TypeTable::object)
                    continue;
                if (TypeID number = testedTypeBesideNumber(reporter, value, value.operand(operandIndex), value.operand(1 - operandIndex)))
                    tested[operandIndex] = number;
            }
        }
        if (!tested[leftOperand] && !tested[rightOperand])
            return;

        std::optional<OperatorTests> found = findFewestTests(reporter, site, value, tested);
        if (!found)
            return;
        if (testsOnlyBoundedCounter(reporter, value, found->isTested, boundedCounters, comparisonsWithTestedBound.span())) {
            reporter.putOffUse();
            return;
        }

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
