/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeTestSite.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTBuiltinTypes.h"
#include "AOTEdgeResults.h"
#include "AOTGenericCopy.h"
#include "AOTInductionVariables.h"
#include "AOTInlineCopy.h"
#include "AOTInlineCostModel.h"
#include "AOTNaturalLoops.h"
#include "AOTOperatorForms.h"
#include "AOTPropertyAccessElements.h"
#include "AOTRepresentation.h"
#include "AOTSort.h"
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
    // Every turn passes the comparison, so what the loop hands on is bounded wherever the step stands: `++i < n`, `i++ < n`, `do .. while (++i < n)`.
    std::optional<unsigned> loopTestedInEveryTurn { };
};

// In the order of their parameters.
using BoundedCounters = Vector<BoundedCounter, 2>;

// What a likely iteration of the loop is better off with.
enum class CounterForm : uint8_t { Integer, Number, Either };

// What the search for bounded counters finds once for the whole function.
struct KnownOfCounters {
    BitVector isComparisonWithTestedBound;
    // By the parameter of the counter: 1 where it is only reckoned with, 2 where not.
    Vector<uint8_t> isOnlyReckonedWith;
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

// Whoever asks whether a value is truthy, or whether it is the same as another, reckons with undefined and null: `if (props.bold)`, `kept !== props.bold` with `bold?: boolean`.
static constexpr OpcodeSet operatorsThatTakeNullish { Opcode::ToBoolean, Opcode::StrictEq, Opcode::Eq };

static TypeID admitExpectedNullish(TestSiteReporter& reporter, ValueID operand, TypeID tested)
{
    TypeArena& types = reporter.types();
    if (!(types.at(reporter.function().at(operand).expectedOrType().predicted()).primitives & Primitive::UndefinedOrNull))
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

// `x | 0` is how a program truncates: it is written where x may be a fraction. The operator converts any number in a few instructions, and a test for an integer fails at every fraction.
static bool isTruncatedFraction(TestSiteReporter& reporter, const Value& user, ValueID operand)
{
    static constexpr OpcodeSet operatorsThatZeroLeavesAlone { Opcode::BitOr, Opcode::BitXor, Opcode::LShift, Opcode::RShift, Opcode::URShift };
    if (!operatorsThatZeroLeavesAlone.contains(user.opcode))
        return false;
    const Type& type = reporter.typeOf(operand);
    if (!type.isNumber() || !(type.primitives & Primitive::FractionalNumber))
        return false;
    bool isCommutative = user.opcode == Opcode::BitOr || user.opcode == Opcode::BitXor;
    ValueID other = user.leftOperand() == operand ? user.rightOperand() : isCommutative ? user.leftOperand() : ValueID();
    if (!other)
        return false;
    const Type& typeOfOther = reporter.typeOf(other);
    return typeOfOther.isInteger() && typeOfOther.integers.isSingleValue() && !typeOfOther.integers.minimum;
}

static TypeID testedTypeFor(TestSiteReporter& reporter, const OperatorSite& site, const Value& user, ValueID operand)
{
    if (testedBitwiseOperators.contains(user.opcode) && (isHeldAsInteger(reporter, site, operand) || isTruncatedFraction(reporter, user, operand)))
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

// An expected type that names no primitive, or leaves out no kind of them, says nothing to an operator. Such an operand is taken for a number where the operand beside it is one, or where what the operator makes is taken for one.
static TypeID testedTypeBesideNumber(TestSiteReporter& reporter, const Value& user, ValueID operand, ValueID other, NOESCAPE const Invocable<bool()> auto& makesNumber)
{
    const Type& type = reporter.typeOf(operand);
    if (!(type.primitives & Primitive::Number) || type.isNumber() || (!isNumberByTypeOrExpectedType(reporter, other) && !makesNumber()))
        return { };
    constexpr uint16_t noPrimitives = 0;
    std::optional<Type> expected = reporter.expectedPartOf(operand, noPrimitives);
    if (expected && expected->primitives && expected->primitives != type.primitives)
        return { };
    // What is compared with a number that may be left out may have been left out too.
    return operatorsThatTakeNullish.contains(user.opcode) ? admitExpectedNullish(reporter, other, TypeTable::number) : TypeTable::number;
}

static bool isBetterForm(Opcode opcode, const OperatorChoice& now, const OperatorChoice& then, bool narrowsWideInteger = false)
{
    if (then.form == OperatorForm::Generic || then.form == now.form)
        return false;
    if (now.form == OperatorForm::Generic)
        return true;
    if (now.effects.normal.mayCallUnknownCode && !then.effects.normal.mayCallUnknownCode)
        return true;
    if (mayCallRuntime(opcode, now.form) && !mayCallRuntime(opcode, then.form))
        return true;
    // With an integer beyond 32 bits the checked form answers with a double, which whoever reckons on converts back.
    if (narrowsWideInteger && now.form == OperatorForm::CheckedIntegers && then.form == OperatorForm::Integers)
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
        bool narrowsWideInteger = false;
        for (uint32_t operandIndex = 0; operandIndex < value.numberOfOperands(); ++operandIndex) {
            const Type& type = reporter.typeOf(value.operand(operandIndex));
            narrowsWideInteger |= isTested[operandIndex] && type.isInteger() && !type.isInt32();
        }
        if (!isBetterForm(value.opcode, best, then, narrowsWideInteger))
            continue;
        best = WTF::move(then);
        chosen = isTested;
    }
    if (!chosen)
        return std::nullopt;
    return OperatorTests { *chosen, best.effects.normal };
}

static constexpr OpcodeSet operationsOnElementsOfViews { Opcode::GetTypedArrayElement, Opcode::PutTypedArrayElement, Opcode::IsInsideView };
static constexpr OpcodeSet operationsOnElementsOfArrays { Opcode::GetArrayElement, Opcode::PutArrayElement, Opcode::HasArrayElement, Opcode::IsElementPresent, Opcode::IsInBounds };
static constexpr OpcodeSet operationsOnElements { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas };
static constexpr uint32_t collectionOperandIndex = 0;
static constexpr uint32_t positionOperandIndex = 1;
static constexpr size_t maxNamesOfCounter = 16;

static Expected<bool> findsElementOfView(TestSiteReporter& reporter, const Use& use)
{
    const Value& user = reporter.function().at(use.user);
    if (use.operandIndex != positionOperandIndex)
        return false;
    if (operationsOnElementsOfViews.contains(user.opcode))
        return true;
    if (!operationsOnElements.contains(user.opcode))
        return false;
    const Type& collection = reporter.program().expectedOrTypeOf(reporter.function(), user.operand(collectionOperandIndex)).predicted();
    return typedArrayTypeOf(BuiltinTable::singleton(), collection.withoutPrimitives(Primitive::UndefinedOrNull)) != NotTypedArray;
}

static Expected<bool> findsElementOfArray(TestSiteReporter& reporter, const Use& use)
{
    const Value& user = reporter.function().at(use.user);
    if (use.operandIndex != positionOperandIndex)
        return false;
    if (operationsOnElementsOfArrays.contains(user.opcode))
        return true;
    if (!operationsOnElements.contains(user.opcode))
        return false;
    std::optional<ElementAccessPlan> plan = planElementAccessOf(reporter.program(), reporter.function(), user);
    return plan && plan->form == ElementAccessForm::Array;
}

// `i < n; i++`.
static bool stepsOnlyBehindTest(TestSiteReporter& reporter, const InductionVariable& variable, const InductionTest& test)
{
    return test.tested == TestedValue::BeforeStep && std::ranges::all_of(variable.steppedValues, [&](ValueID stepped) {
        return dominatesAfterSplit(reporter.analyses(), test.stayingEdge, reporter.analyses().useLists().blockOf(stepped));
    });
}

// Whether typed code in the loop does nothing with the counter but reckon, compare and find elements of views. A call, a store or a return wants a value, and to make one of an integer beyond int32 takes more code than the test does. Behind the loop the value is made once, and the test would be made at every turn.
// An array counts its elements in 32 bits. `i < values.length; i++` asks its counter for an int32 once a turn, behind everything else, which takes no time. Where the step stands in front of the test or the test looks at what was counted up, the counter is asked at the head of the turn and again where it finds the element, and the back end takes the loop for one without a bound: there an array gets an integer, which it compares with its length as it compares an int32.
static Expected<bool> isOnlyReckonedWith(TestSiteReporter& reporter, const NaturalLoop& loop, const InductionVariable& variable)
{
    bool mayFindElementsOfArrays = std::ranges::any_of(variable.tests, [&](const InductionTest& test) {
        return isBoundingRelation(test.relation, variable.direction) && !stepsOnlyBehindTest(reporter, variable, test);
    });
    const Function& function = reporter.function();
    const UseLists& useLists = reporter.analyses().useLists();
    const NaturalLoops& loops = reporter.analyses().naturalLoops();
    // `while (++i < n) use(i)`: the body sees what the turn has counted up.
    Vector<ValueID, maxNamesOfCounter> names { variable.parameter };
    for (ValueID stepped : variable.steppedValues) {
        if (names.size() == maxNamesOfCounter)
            return false;
        if (!names.contains(stepped))
            names.append(stepped);
    }
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
            if (function.at(use.block).frequency.predicted() != BlockFrequency::Normal || !loops.belongsTo(use.block, loop))
                continue;
            switch (use.kind) {
            case Use::Kind::Operand: {
                Opcode opcode = function.at(use.user).opcode;
                // `i++` converts what it counts up, and a number is converted to itself.
                bool isConversionOfNumber = (opcode == Opcode::ToNumeric || opcode == Opcode::ToNumber) && reporter.typeOf(names[index]).isNumber();
                if (isCastWithoutTest(opcode) || isConversionOfNumber) {
                    if (!follow(use.user))
                        return false;
                    continue;
                }
                if (opcode == Opcode::TypeTest || numericOperators.contains(opcode) || findsElementOfView(reporter, use).predicted())
                    continue;
                if (mayFindElementsOfArrays && findsElementOfArray(reporter, use).predicted())
                    continue;
                if (opcode == Opcode::Add && reporter.typeOf(use.user).isNumber())
                    continue;
                return false;
            }
            case Use::Kind::EdgeArgument: {
                BlockID targetBlock = targetOf(function, use.edge()).block;
                const Block& target = function.at(targetBlock);
                if (target.frequency.predicted() == BlockFrequency::Normal && loops.belongsTo(targetBlock, loop) && (use.operandIndex >= target.parameters.size() || !follow(target.parameters[use.operandIndex])))
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
static BoundedCounters findBoundedCounters(TestSiteReporter& reporter, KnownOfCounters& known)
{
    BoundedCounters counters;
    if (known.isOnlyReckonedWith.isEmpty())
        known.isOnlyReckonedWith.fill(0, reporter.function().values.size());
    for (const LoopInduction& loop : reporter.analyses().inductionVariables().loops()) {
        for (const InductionVariable& variable : loop.variables) {
            if (variable.step.kind != InductionStepKind::Constant || !variable.start || !reporter.typeOf(variable.start).isInteger())
                continue;
            std::optional<bool> isEligible;
            for (const InductionTest& test : variable.tests) {
                if (!isBoundingRelation(test.relation, variable.direction))
                    continue;
                // ConvertCountersToIntegers passes on what such a comparison says on the edge that is taken.
                std::optional<unsigned> loopTestedInEveryTurn;
                if (test.exits != LoopExits::Unknown && !test.stayingEdge.targetIndex)
                    loopTestedInEveryTurn = loop.loopIndex;
                if (test.tested != TestedValue::BeforeStep && !loopTestedInEveryTurn)
                    continue;
                if (known.isComparisonWithTestedBound.get(test.comparison.index())) {
                    counters.append({ variable.parameter, test.comparison, test.stayingEdge, loopTestedInEveryTurn });
                    continue;
                }
                const Type& bound = reporter.typeOf(test.bound);
                if (!(bound.isInteger() && !bound.isInt32()) && !reporter.isClaimed(reporter.analyses().identityOf(test.bound)))
                    continue;
                if (!isEligible) {
                    uint8_t& answer = known.isOnlyReckonedWith[variable.parameter.index()];
                    if (!answer)
                        answer = isOnlyReckonedWith(reporter, reporter.analyses().naturalLoops().loop(loop.loopIndex), variable).predicted() ? 1 : 2;
                    isEligible = answer == 1;
                }
                if (*isEligible)
                    counters.append({ variable.parameter, test.comparison, test.stayingEdge, loopTestedInEveryTurn });
            }
        }
    }
    AOT::stableSort(counters, { }, &BoundedCounter::parameter);
    return counters;
}

// The block parameter that a number is, or was before it was converted to the number it is and constants were added to it: `i - 1` behind `i++ < n`.
static ValueID findParameterReckonedFrom(TestSiteReporter& reporter, ValueID id)
{
    constexpr unsigned mostSteps = 4;
    const Function& function = reporter.function();
    for (unsigned step = 0;; ++step) {
        id = reporter.analyses().identityOf(id);
        if (reporter.analyses().useLists().locationOf(id).placement == ValuePlacement::Parameter || step == mostSteps)
            return id;
        const Value& value = function.at(id);
        auto isConstant = [&](uint32_t operandIndex) {
            return !!integerConstantThroughCasts(reporter.program(), function, value.operand(operandIndex));
        };
        uint32_t reckonedFrom = 0;
        switch (value.opcode) {
        case Opcode::ToNumber:
        case Opcode::ToNumeric:
        case Opcode::Increment:
        case Opcode::Decrement:
            break;
        case Opcode::Add:
            if (isConstant(rightOperand))
                break;
            if (!isConstant(leftOperand))
                return id;
            reckonedFrom = rightOperand;
            break;
        case Opcode::Sub:
            if (!isConstant(rightOperand))
                return id;
            break;
        default:
            return id;
        }
        if (!reporter.typeOf(value.operand(reckonedFrom)).isNumber())
            return id;
        id = value.operand(reckonedFrom);
    }
}

// An operator takes an integer as it is, so none of these needs a test. One for int32 that is placed here stays in the loop for good: a view may be longer than that, and nothing proves that this one is not.
static bool testsOnlyBoundedCounter(TestSiteReporter& reporter, const Value& value, const TestedOperands& isTested, std::optional<BoundedCounters>& boundedCounters, KnownOfCounters& known)
{
    // Behind the rounds the counters that could be converted have been.
    if (reporter.context().isBehindRounds || !reporter.analyses().blockCycles().innermostCycleOf(reporter.point().block))
        return false;
    if (!boundedCounters)
        boundedCounters = findBoundedCounters(reporter, known);
    // Every tested operand has to be the counter, or a constant away from it.
    std::span<const BoundedCounter> counters = boundedCounters->span();
    for (uint32_t operandIndex = 0; operandIndex < value.numberOfOperands(); ++operandIndex) {
        if (isTested[operandIndex])
            counters = std::span(std::ranges::equal_range(counters, findParameterReckonedFrom(reporter, value.operand(operandIndex)), { }, &BoundedCounter::parameter));
    }
    const NaturalLoops& loops = reporter.analyses().naturalLoops();
    return std::ranges::any_of(counters, [&](const BoundedCounter& counter) {
        if (counter.loopTestedInEveryTurn && loops.belongsTo(reporter.point().block, loops.loop(*counter.loopTestedInEveryTurn)))
            return true;
        return reporter.use() == counter.comparison || dominatesAfterSplit(reporter.analyses(), counter.stayingEdge, reporter.point().block);
    });
}

static bool isExpectedToBeIntegerOfUnknownSize(TestSiteReporter& reporter, ValueID operand)
{
    std::optional<Type> expected = reporter.expectedPartOf(operand);
    return expected && expected->isInteger() && expected->integers == Range::full();
}

static bool isAccessUnderTests(TestSiteReporter& reporter, ValueID operand)
{
    ValueID identity = reporter.analyses().identityOf(operand);
    const Value& access = reporter.function().at(identity);
    if (accessClassOf(access.opcode) == AccessClass::None || !reporter.isClaimed(identity))
        return false;
    // The test that was asked for here is not placed.
    return !isNamedAccess(access.opcode) || !arrivesWithoutLayoutOfProgram(reporter.context(), reporter.analyses().useLists().blockOf(identity), access.operand(*receiverOf(access.opcode)));
}

// What a field, an array or a variable of numbers holds arrives as a number. An access for which a rule in front of this one has just asked for tests delivers behind them what is expected of it.
static bool arrivesAsValueOfAnyType(TestSiteReporter& reporter, ValueID operand)
{
    const Value& definition = reporter.function().at(reporter.analyses().identityOf(operand));
    return defaultRepresentationOf(reporter.types().at(isAccessUnderTests(reporter, operand) ? definition.expectedOrType().predicted() : definition.type)) == Representation::JSValue;
}

static constexpr OpcodeSet sums { Opcode::Add, Opcode::Sub };

// Of a sum of int32s a bitwise operator takes the low half. A sum with an integer of unknown size is a number, and to convert that costs more than the test.
static bool makesSumForBitwiseOperators(TestSiteReporter& reporter, const OperatorSite& site, ValueID id, uint32_t operandIndex)
{
    const Value& user = reporter.function().at(id);
    if (!sums.contains(user.opcode))
        return false;
    TestedTypePerOperand tested;
    TestedOperands isTested { };
    tested[operandIndex] = TypeTable::int32;
    isTested[operandIndex] = true;
    if (chooseFormUnderTests(reporter, site, user, tested, isTested).form != OperatorForm::Integers)
        return false;

    const Function& function = reporter.function();
    Vector<ValueID, maxNamesOfCounter> names { id };
    bool isTaken = false;
    for (size_t index = 0; index < names.size(); ++index) {
        for (const Use& use : reporter.analyses().useLists().usesOf(names[index])) {
            ValueID next;
            if (use.kind == Use::Kind::EdgeArgument) {
                const Block& target = function.at(targetOf(function, use.edge()).block);
                if (use.operandIndex < target.parameters.size())
                    next = target.parameters[use.operandIndex];
            } else if (use.kind == Use::Kind::Operand) {
                Opcode opcode = function.at(use.user).opcode;
                if (testedBitwiseOperators.contains(opcode)) {
                    isTaken = true;
                    continue;
                }
                if (isCastWithoutTest(opcode) || sums.contains(opcode))
                    next = use.user;
            }
            if (!next)
                return false;
            if (names.contains(next))
                continue;
            if (names.size() == maxNamesOfCounter)
                return false;
            names.append(next);
        }
    }
    return isTaken;
}

// Of an integer of which only its kind is known, few are beyond int32, and an operator on int32 needs no test of its result. So a test for int32 takes the place of the test for a number that the operand gets anyway. In a value of any type an int32 is found as fast as a number, and is taken out faster. In a number it takes three to six instructions more, which most operators do not save.
static TypeID narrowTestOfIntegerOfUnknownSize(TestSiteReporter& reporter, const OperatorSite& site, ValueID id, uint32_t operandIndex, TypeID tested)
{
    const Value& user = reporter.function().at(id);
    ValueID operand = user.operand(operandIndex);
    if (!tested || !isExpectedToBeIntegerOfUnknownSize(reporter, operand))
        return tested;
    if (!arrivesAsValueOfAnyType(reporter, operand) && !makesSumForBitwiseOperators(reporter, site, id, operandIndex))
        return tested;
    return operatorsThatTakeNullish.contains(user.opcode) ? admitExpectedNullish(reporter, operand, TypeTable::int32) : TypeTable::int32;
}

static constexpr OpcodeSet operatorsThatMakeNumbersOfNumbers { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div, Opcode::Mod, Opcode::Pow };

// A test that the compiler placed: the side on which it fails is not a likely one. `typeof x === "number"` in the program reckons with both.
static bool isExpectedToPass(TestSiteReporter& reporter, ValueID test)
{
    const Function& function = reporter.function();
    return std::ranges::any_of(reporter.analyses().useLists().usesOf(test), [&](const Use& use) {
        if (use.kind != Use::Kind::TerminatorOperand || !function.at(use.block).terminator.isConditionalBranch())
            return false;
        return function.at(function.at(use.block).terminator.notTaken().block).frequency.predicted() != BlockFrequency::Normal;
    });
}

// `sum += shape.width + shape.height`, with shapes of which nothing is known. What likely code adds to a number, compares with one or tests for one is taken for a number, and it is one only where both operands are.
// Unlikely code is where such a test has failed already.
static bool isTakenForNumber(TestSiteReporter& reporter, ValueID result)
{
    const Function& function = reporter.function();
    BlockID block = reporter.analyses().useLists().blockOf(result);
    if (!block || function.at(block).frequency.predicted() != BlockFrequency::Normal || !(reporter.typeOf(result).primitives & Primitive::Number))
        return false;
    Vector<ValueID, maxNamesOfCounter> names { result };
    for (size_t index = 0; index < names.size(); ++index) {
        std::optional<Type> expected = reporter.expectedPartOf(names[index]);
        if (expected && expected->isNumber())
            return true;
        for (const Use& use : reporter.analyses().useLists().usesOf(names[index])) {
            if (function.at(use.block).frequency.predicted() != BlockFrequency::Normal)
                continue;
            ValueID next;
            if (use.kind == Use::Kind::EdgeArgument) {
                const Block& target = function.at(targetOf(function, use.edge()).block);
                if (target.frequency.predicted() == BlockFrequency::Normal && use.operandIndex < target.parameters.size())
                    next = target.parameters[use.operandIndex];
            } else if (use.kind == Use::Kind::Operand) {
                const Value& user = function.at(use.user);
                if (user.opcode == Opcode::TypeTest && reporter.types().at(user.testedType()).isNumber() && isExpectedToPass(reporter, use.user))
                    return true;
                if (hasOperatorForm(user.opcode) && user.numberOfOperands() == 2 && isNumberByTypeOrExpectedType(reporter, user.operand(1 - use.operandIndex)))
                    return true;
                if (isCastWithoutTest(user.opcode))
                    next = use.user;
            }
            if (next && !names.contains(next) && names.size() < maxNamesOfCounter)
                names.append(next);
        }
    }
    return false;
}

static TestedTypePerOperand testedTypesOfOperands(TestSiteReporter& reporter, const OperatorSite& site, ValueID id)
{
    const Value& value = reporter.function().at(id);
    uint32_t numberOfOperands = value.numberOfOperands();
    TestedTypePerOperand tested;
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex)
        tested[operandIndex] = narrowTestOfIntegerOfUnknownSize(reporter, site, id, operandIndex, testedTypeFor(reporter, site, value, value.operand(operandIndex)));
    if (numberOfOperands == 2) {
        std::optional<bool> isTaken;
        // What a read delivers behind the tests that it has just got shows in the next round.
        auto makesNumber = [&] {
            if (!isTaken) {
                isTaken = operatorsThatMakeNumbersOfNumbers.contains(value.opcode) && !isAccessUnderTests(reporter, value.operand(leftOperand))
                    && !isAccessUnderTests(reporter, value.operand(rightOperand)) && isTakenForNumber(reporter, id);
            }
            return *isTaken;
        };
        for (uint32_t operandIndex : { leftOperand, rightOperand }) {
            if (tested[operandIndex] && tested[operandIndex] != TypeTable::object)
                continue;
            if (TypeID number = testedTypeBesideNumber(reporter, value, value.operand(operandIndex), value.operand(1 - operandIndex), makesNumber))
                tested[operandIndex] = number;
        }
    }
    return tested;
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

static void forEachValueNarrowedByBranchOn(const Function& function, const UseLists& useLists, ValueID test, ValueID operand, NOESCAPE const Invocable<void(ValueID)> auto& functor)
{
    for (const Use& use : useLists.usesOf(test)) {
        if (use.kind != Use::Kind::TerminatorOperand || !function.at(use.block).terminator.isConditionalBranch())
            continue;
        const Terminator& branch = function.at(use.block).terminator;
        for (uint32_t targetIndex = 0; targetIndex < branch.targets.size(); ++targetIndex) {
            const Target& target = branch.targets[targetIndex];
            if (target.proofs.isEmpty())
                continue;
            uint32_t firstParameter = firstEdgeResultParameter(function, use.block, targetIndex);
            for (uint32_t index = 0; index < target.proofs.size(); ++index) {
                if (operandNarrowedOnEdge(function, { use.block, targetIndex, index }) == operand)
                    functor(function.at(target.block).parameters[firstParameter + index]);
            }
        }
    }
}

// Integer where a likely turn of the loop does something with the counter that an integer is better for: an operator gets a better form, or the counter finds an element or goes into a built-in function, or into a call: an integer stays in its register around the call and takes one instruction to become a value of any type. Whatever else takes it, a number that it is added to or a variable of numbers, takes it as a number for nothing. Either where nothing takes it but its step, its bound and sums that only go round the loop.
static Expected<CounterForm> formWantedOf(TestSiteReporter& reporter, const OperatorSite& site, const NaturalLoop& loop, const InductionVariable& counter)
{
    const Function& function = reporter.function();
    const NaturalLoops& loops = reporter.analyses().naturalLoops();
    Vector<ValueID, maxNamesOfCounter> names { counter.parameter };
    bool isTakenAsNumber = false;
    auto isOrBecomesInteger = [&](ValueID operand) {
        const Type& type = reporter.typeOf(operand);
        if (type.isInteger() || !type.isNumberWithoutFraction())
            return type.isInteger();
        ValueLocation location = reporter.analyses().useLists().locationOf(reporter.analyses().identityOf(operand));
        return location.placement == ValuePlacement::Parameter && loops.belongsTo(location.block, loop);
    };
    // What is reckoned from the counter.
    Vector<ValueID, maxNamesOfCounter> reckoned;
    auto follow = [](Vector<ValueID, maxNamesOfCounter>& values, ValueID value) {
        if (!values.contains(value) && values.size() < maxNamesOfCounter)
            values.append(value);
        return values.contains(value);
    };
    for (size_t index = 0; index < names.size(); ++index) {
        for (const Use& use : reporter.analyses().useLists().usesOf(names[index])) {
            if (use.kind != Use::Kind::Operand || function.at(use.block).frequency.predicted() != BlockFrequency::Normal || !loops.belongsTo(use.block, loop) || counter.steppedValues.contains(use.user))
                continue;
            const Value& user = function.at(use.user);
            if (isCastWithoutTest(user.opcode)) {
                // A function that takes the counter takes an integer, which flow knows it to be however far it counts. Inlined, its body reckons with integers beyond 32 bits, and with an int32 where the counter is one.
                if (reporter.typeOf(use.user).isInteger() && !reporter.typeOf(names[index]).isInteger())
                    return CounterForm::Integer;
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
                    return CounterForm::Integer;
                // `field + counter`: the operand beside the counter gets a test of its own once that makes a better form, which it does beside an integer alone.
                uint32_t other = 1 - use.operandIndex;
                if (user.numberOfOperands() == 2 && (tested[other] = testedTypesOfOperands(reporter, site, use.user)[other])) {
                    isTested = { };
                    isTested[other] = true;
                    OperatorChoice besideNumber = chooseFormUnderTests(reporter, site, user, tested, isTested);
                    isTested[use.operandIndex] = true;
                    if (isBetterForm(user.opcode, besideNumber, chooseFormUnderTests(reporter, site, user, tested, isTested)))
                        return CounterForm::Integer;
                }
                // Beside a number that is held as a double an integer is converted in every iteration. A sum without a fraction that goes round the loop becomes an integer with the counter.
                if (!relationalOperators.contains(user.opcode)) {
                    isTakenAsNumber |= user.numberOfOperands() == 2 && !isOrBecomesInteger(user.operand(other));
                    isTakenAsNumber |= !follow(reckoned, use.user);
                }
                forEachValueNarrowedByBranchOn(function, reporter.analyses().useLists(), use.user, names[index], [&](ValueID narrowed) {
                    if (!names.contains(narrowed) && names.size() < maxNamesOfCounter)
                        names.append(narrowed);
                });
                continue;
            }
            OperandKind kind = user.operandKind(use.operandIndex);
            if (kind == OperandKind::Number || kind == OperandKind::Integer || user.opcode == Opcode::TypeTest || user.opcode == Opcode::CallBuiltin)
                return CounterForm::Integer;
            if (user.opcode == Opcode::Call || (user.opcode == Opcode::CallDirect && staysCall(reporter, use.block, use.user)))
                return CounterForm::Integer;
            if (operationsOnElements.contains(user.opcode) && use.operandIndex == positionOperandIndex)
                return CounterForm::Integer;
            isTakenAsNumber = true;
        }
    }
    // It goes round the loop, where it becomes an integer with the counter. Whatever else takes it, takes it as a number.
    for (size_t index = 0; index < reckoned.size() && !isTakenAsNumber; ++index) {
        for (const Use& use : reporter.analyses().useLists().usesOf(reckoned[index])) {
            if (use.kind != Use::Kind::Operand || function.at(use.block).frequency.predicted() != BlockFrequency::Normal || !loops.belongsTo(use.block, loop))
                continue;
            isTakenAsNumber |= !isCastWithoutTest(function.at(use.user).opcode) || !follow(reckoned, use.user);
        }
    }
    return isTakenAsNumber ? CounterForm::Number : CounterForm::Either;
}

static bool startsAtInt32(TestSiteReporter& reporter, const NaturalLoop& loop, const InductionVariable& counter, const InductionTest& test)
{
    if (reporter.typeOf(counter.start).isInt32())
        return true;
    const Function& function = reporter.function();
    FunctionAnalyses& analyses = reporter.analyses();
    const NaturalLoop* waysBack = findLoopOfWaysBackInto(analyses, loop);
    ValueLocation join = analyses.useLists().locationOf(analyses.identityOf(counter.start));
    if (!waysBack || join.placement != ValuePlacement::Parameter || join.block != waysBack->header())
        return false;
    BlockID whereBoundIsMade = analyses.useLists().blockOf(analyses.identityOf(test.bound));
    if (!whereBoundIsMade || analyses.naturalLoops().belongsTo(whereBoundIsMade, *waysBack))
        return false;
    return std::ranges::all_of(analyses.cfg().incomingEdges(join.block), [&](Edge edge) {
        if (edge.isHandlerEdge() || join.index >= numberOfExplicitArguments(function, edge))
            return false;
        ValueID entering = analyses.identityOf(valuePassedOn(function, edge, join.index));
        if (reporter.typeOf(entering).isInt32())
            return true;
        BlockID whereCounterIsNamed = entering == counter.parameter ? edge.from : analyses.useLists().blockOf(entering);
        return findParameterReckonedFrom(reporter, entering) == counter.parameter && dominatesAfterSplit(analyses, test.stayingEdge, whereCounterIsNamed);
    });
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
    auto holdsLoop = [&] {
        reporter.context().build.statistics().count(StatisticID::BlocksReadForLoopsInsideLoops, loop->size());
        for (unsigned index = 0; index < loop->size(); ++index) {
            if (loops.innerMostLoopOf(loop->at(index)) != loop)
                return true;
        }
        return false;
    };

    for (const InductionVariable& variable : reporter.analyses().inductionVariables().of(*loop).variables) {
        if (variable.step.kind != InductionStepKind::Constant || !variable.start)
            continue;
        for (const InductionTest& test : variable.tests) {
            if (test.comparison != reporter.use() || test.tested != TestedValue::BeforeStep || test.boundKind != LoopBoundKind::Invariant || !isBoundingRelation(test.relation, variable.direction))
                continue;
            if (holdsLoop())
                return std::nullopt;
            if (!startsAtInt32(reporter, *loop, variable, test))
                break;
            ValueID bound = reporter.analyses().identityOf(test.bound);
            bool isLeftBound = reporter.analyses().identityOf(comparison.operand(leftOperand)) == bound;
            bool isRightBound = reporter.analyses().identityOf(comparison.operand(rightOperand)) == bound;
            if (isLeftBound == isRightBound || !mayBeBound(isLeftBound ? leftOperand : rightOperand))
                return std::nullopt;
            // An integer steps in one cycle, and a loop that only counts can go. But the test copies the loop: where nothing asks for an integer it waits until nothing is inlined, expanded or sunk any more.
            CounterForm wanted = formWantedOf(reporter, site, *loop, variable).predicted();
            if (wanted != CounterForm::Integer && !reporter.context().isBehindRounds) {
                reporter.putOffUse();
                return std::nullopt;
            }
            if (wanted == CounterForm::Number)
                return std::nullopt;
            return isLeftBound ? leftOperand : rightOperand;
        }
    }
    return std::nullopt;
}

// `while (i--)`: a counter that goes down by one until it is zero starts at zero or above, or the loop does not end. Asked for that, it is never negative, and then that it is not zero says that it is above zero: a bound like any other, for ConvertCountersToIntegers and for the back end.
static bool countsDownUntilZero(TestSiteReporter& reporter, const Value& conversion)
{
    if (conversion.opcode != Opcode::ToNumeric && conversion.opcode != Opcode::ToNumber)
        return false;
    const Function& function = reporter.function();
    FunctionAnalyses& analyses = reporter.analyses();
    const NaturalLoops& loops = analyses.naturalLoops();
    const NaturalLoop* loop = loops.innerMostLoopOf(reporter.point().block);
    if (!loop)
        return false;
    ValueLocation counter = analyses.useLists().locationOf(analyses.identityOf(conversion.operand(0)));
    if (counter.placement != ValuePlacement::Parameter || counter.block != loop->header())
        return false;

    ValueID number = reporter.use();
    bool everyTurnStepsDownByOne = std::ranges::all_of(analyses.cfg().incomingEdges(loop->header()), [&](Edge edge) {
        if (!loops.belongsTo(edge.from, *loop))
            return true;
        ValueID handedOn = valuePassedOn(function, edge, counter.index);
        if (!handedOn)
            return false;
        const Value& step = function.at(analyses.identityOf(handedOn));
        return step.opcode == Opcode::Decrement && analyses.identityOf(step.operand(0)) == number;
    });
    if (!everyTurnStepsDownByOne)
        return false;
    return std::ranges::any_of(analyses.useLists().usesOf(number), [&](const Use& use) {
        if (use.kind != Use::Kind::Operand || function.at(use.user).opcode != Opcode::ToBoolean)
            return false;
        return std::ranges::any_of(analyses.useLists().usesOf(use.user), [&](const Use& test) {
            if (test.kind != Use::Kind::TerminatorOperand || !function.at(test.block).terminator.isConditionalBranch())
                return false;
            const Terminator& branch = function.at(test.block).terminator;
            return loops.belongsTo(test.block, *loop) && loops.belongsTo(branch.taken().block, *loop) && !loops.belongsTo(branch.notTaken().block, *loop);
        });
    });
}

// The one rule behind this one looks at tests of a type, so a site that is left out here is claimed from nobody.
static_assert(!hasOperatorForm(Opcode::TypeTest));

void reportOperatorTests(TestSiteReporter& reporter)
{
    TypingContext typing { reporter.program(), reporter.function(), reporter.types() };
    OperatorSite site { typing, reporter.analyses().effectContext(), CodeShape::Fast };

    std::optional<BoundedCounters> boundedCounters;
    KnownOfCounters knownOfCounters;
    reporter.forEachCandidate(Value::opcodesWithForm, [&](const Value& value) {
        if (!hasOperatorForm(value.opcode) || reporter.takesAwaySitesInColdCode())
            return;
        uint32_t numberOfOperands = value.numberOfOperands();
        RELEASE_ASSERT(numberOfOperands == 1 || numberOfOperands == 2);

        if (std::optional<uint32_t> bound = findNumberThatBoundsCounter(reporter, site, value)) {
            reporter.report({ makeTypeTest(value.operand(*bound), TypeTable::int32) }, chooseOperatorForm(site, value).effects.normal);
            reporter.context().hasBoundsTestedBehindRounds |= reporter.context().isBehindRounds;
            knownOfCounters.isComparisonWithTestedBound.set(reporter.use().index());
            boundedCounters = std::nullopt;
            return;
        }

        TestedTypePerOperand tested = testedTypesOfOperands(reporter, site, reporter.use());
        if (!tested[leftOperand] && !tested[rightOperand])
            return;
        if (tested[leftOperand] == TypeTable::int32 && countsDownUntilZero(reporter, value)) {
            Range zeroOrAbove = Range::int32();
            zeroOrAbove.minimum = 0;
            tested[leftOperand] = reporter.types().intern(Type::integer(zeroOrAbove));
        }

        std::optional<OperatorTests> found = findFewestTests(reporter, site, value, tested);
        if (!found)
            return;
        if (testsOnlyBoundedCounter(reporter, value, found->isTested, boundedCounters, knownOfCounters)) {
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
