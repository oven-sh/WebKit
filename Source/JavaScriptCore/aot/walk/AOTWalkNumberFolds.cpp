/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Folding, of a call that goes by one row of a built-in on numbers: `Math.max(1, 2)`, `Math.abs(-3)`, `Number.isInteger(i)`, `parseInt("ff", 16)`.
// Only what IEEE 754 and ECMAScript decide to the last bit. No function of the C library whose last bit differs between machines is ever called: sin, exp, log, pow and their kind stay calls.
// It computes and builds nothing. At a suspension and at a throw: nothing. Withheld: the call goes by its row.

#include "config.h"
#include "AOTWalkNumberForms.h"

#if ENABLE(AOT)

#include "AOTTypeArena.h"
#include "JSGlobalObjectFunctions.h"
#include "MathCommon.h"
#include "ParseInt.h"
#include <bit>
#include <cmath>
#include <limits>
#include <wtf/Float16.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class TestOfNumberInFolds : uint8_t { IsNaN, IsFinite, IsInteger, IsSafeInteger };

struct TestAskedInFolds {
    TestOfNumberInFolds test;
    // isNaN() and isFinite() of the global object. Those of Number say false of what is no number.
    bool converts { false };
};

} // anonymous namespace

static constexpr int64_t greatestSafeIntegerInFolds = (int64_t { 1 } << 53) - 1;

static std::optional<TestAskedInFolds> testOfNumberAskedInFolds(InlineBuiltin builtin)
{
    switch (builtin) {
    case InlineBuiltin::IsNaN:
        return TestAskedInFolds { TestOfNumberInFolds::IsNaN, true };
    case InlineBuiltin::IsFinite:
        return TestAskedInFolds { TestOfNumberInFolds::IsFinite, true };
    case InlineBuiltin::NumberIsNaN:
        return TestAskedInFolds { TestOfNumberInFolds::IsNaN, false };
    case InlineBuiltin::NumberIsFinite:
        return TestAskedInFolds { TestOfNumberInFolds::IsFinite, false };
    case InlineBuiltin::NumberIsInteger:
        return TestAskedInFolds { TestOfNumberInFolds::IsInteger, false };
    case InlineBuiltin::NumberIsSafeInteger:
        return TestAskedInFolds { TestOfNumberInFolds::IsSafeInteger, false };
    default:
        return std::nullopt;
    }
}

// ToNumber of a known primitive.
static std::optional<double> numberOfKnownPrimitiveInFolds(Walker& walker, PrimitiveKnownToFolding known)
{
    switch (known.kind()) {
    case PrimitiveKnownToFolding::Kind::Undefined:
        return PNaN;
    case PrimitiveKnownToFolding::Kind::Null:
        return 0;
    case PrimitiveKnownToFolding::Kind::Boolean:
        return known.asBoolean() ? 1 : 0;
    case PrimitiveKnownToFolding::Kind::Number:
        return known.asNumber();
    case PrimitiveKnownToFolding::Kind::Text: {
        Vector<char16_t> buffer;
        return jsToNumber(walker.types().viewOf(known.asText(), buffer));
    }
    case PrimitiveKnownToFolding::Kind::Unknown:
        break;
    }
    return std::nullopt;
}

static bool numberPassesTestInFolds(TestOfNumberInFolds test, double number)
{
    switch (test) {
    case TestOfNumberInFolds::IsNaN:
        return std::isnan(number);
    case TestOfNumberInFolds::IsFinite:
        return std::isfinite(number);
    case TestOfNumberInFolds::IsInteger:
        return std::isfinite(number) && std::trunc(number) == number;
    case TestOfNumberInFolds::IsSafeInteger:
        return std::isfinite(number) && std::trunc(number) == number && std::abs(number) <= static_cast<double>(greatestSafeIntegerInFolds);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Of a value that is not known: the answer where every value of the type gets the same. -0, NaN, the infinities and what lies beyond the tracked integers are one primitive, which passes and fails each test.
static std::optional<bool> answerOfTestByTypeInFolds(TestAskedInFolds asked, const Type& type)
{
    if (type.isNever() || !type.isJavaScriptValue() || type.mayBeUnspecified() || type.primitives & Primitive::Absent)
        return std::nullopt;
    bool mayBeNoNumber = type.hasObjects() || type.primitives & ~Primitive::Number;
    if (mayBeNoNumber && asked.converts)
        return std::nullopt;

    bool mayPass = false;
    bool mayFail = mayBeNoNumber;
    if (type.primitives & Primitive::WholeNumber) {
        mayPass = true;
        mayFail = true;
    }
    if (type.primitives & Primitive::FractionalNumber) {
        if (asked.test == TestOfNumberInFolds::IsFinite)
            mayPass = true;
        else
            mayFail = true;
    }
    if (type.primitives & Primitive::Integer) {
        switch (asked.test) {
        case TestOfNumberInFolds::IsNaN:
            mayFail = true;
            break;
        case TestOfNumberInFolds::IsFinite:
        case TestOfNumberInFolds::IsInteger:
            mayPass = true;
            break;
        case TestOfNumberInFolds::IsSafeInteger:
            mayPass |= type.integers.minimum <= greatestSafeIntegerInFolds && type.integers.maximum >= -greatestSafeIntegerInFolds;
            mayFail |= type.integers.minimum < -greatestSafeIntegerInFolds || type.integers.maximum > greatestSafeIntegerInFolds;
            break;
        }
    }
    if (mayPass == mayFail)
        return std::nullopt;
    return mayPass;
}

// A NaN that the processor makes has a sign on some processors and none on others, and the program can read the sign out of a Float64Array. A NaN that an operand brings goes through as it is.
static PrimitiveKnownToFolding numberComputedInFolds(double result, std::span<const double> operands)
{
    if (std::isnan(result)) {
        bool someOperandIsNaN = false;
        for (double operand : operands)
            someOperandIsNaN |= std::isnan(operand);
        if (!someOperandIsNaN)
            return { };
    }
    return PrimitiveKnownToFolding::number(result);
}

// Math.round(-0.2) is -0, and Math.round(2.5) is 3.
static double roundedAsMathRoundDoesInFolds(double number)
{
    double integer = std::ceil(number);
    // Half below a whole number is exact. What lies between the whole number and the operand is not: of 0.49999999999999994 it rounds to a half.
    return integer - (integer - 0.5 > number);
}

static std::optional<double> resultOfExactFunctionInFolds(InlineBuiltin builtin, double number)
{
    switch (builtin) {
    case InlineBuiltin::MathAbs:
        return std::abs(number);
    case InlineBuiltin::MathCeil:
        return std::ceil(number);
    case InlineBuiltin::MathFloor:
        return std::floor(number);
    case InlineBuiltin::MathTrunc:
        return std::trunc(number);
    case InlineBuiltin::MathRound:
        return roundedAsMathRoundDoesInFolds(number);
    case InlineBuiltin::MathSqrt:
        return std::sqrt(number);
    case InlineBuiltin::MathSign:
        if (std::isnan(number) || !number)
            return number;
        return std::signbit(number) ? -1 : 1;
    case InlineBuiltin::MathFround:
        return static_cast<double>(static_cast<float>(number));
    case InlineBuiltin::MathF16Round:
        return static_cast<double>(Float16 { number });
    case InlineBuiltin::MathClz32:
        return std::countl_zero(toUInt32(number));
    default:
        return std::nullopt;
    }
}

// Math.max(0, -0) is 0 and Math.min(0, -0) is -0.
static double strongerOfTwoInFolds(bool asksForGreatest, double left, double right)
{
    if (std::isnan(left))
        return left;
    if (std::isnan(right))
        return right;
    if (left == right)
        return std::signbit(left) == asksForGreatest ? right : left;
    return (left > right) == asksForGreatest ? left : right;
}

static PrimitiveKnownToFolding foldCallOnKnownNumbersInFolds(InlineBuiltin builtin, std::span<const double> numbers)
{
    switch (builtin) {
    case InlineBuiltin::MathImul:
        if (numbers.size() < 2)
            return { };
        return PrimitiveKnownToFolding::integer(static_cast<int32_t>(static_cast<uint32_t>(toInt32(numbers[0])) * static_cast<uint32_t>(toInt32(numbers[1]))));
    case InlineBuiltin::MathMin:
    case InlineBuiltin::MathMax: {
        bool asksForGreatest = builtin == InlineBuiltin::MathMax;
        double strongest = asksForGreatest ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
        for (double number : numbers)
            strongest = strongerOfTwoInFolds(asksForGreatest, strongest, number);
        return PrimitiveKnownToFolding::number(strongest);
    }
    case InlineBuiltin::MathHypot:
        // Of more numbers the sum of the squares is rounded, and in which order is the engine's to choose.
        if (numbers.empty())
            return PrimitiveKnownToFolding::integer(0);
        return numbers.size() == 1 ? PrimitiveKnownToFolding::number(std::abs(numbers[0])) : PrimitiveKnownToFolding();
    default:
        break;
    }
    if (numbers.empty())
        return { };
    std::optional<double> result = resultOfExactFunctionInFolds(builtin, numbers[0]);
    return result ? numberComputedInFolds(*result, numbers.first(1)) : PrimitiveKnownToFolding();
}

// `parseInt("ff", 16)`, `parseInt("12px")`.
static PrimitiveKnownToFolding foldParseIntInFolds(Walker& walker, std::span<const PrimitiveKnownToFolding> known)
{
    if (known.empty() || !known[0].isText())
        return { };
    int32_t radix = 0;
    if (known.size() > 1) {
        std::optional<double> written = numberOfKnownPrimitiveInFolds(walker, known[1]);
        if (!written)
            return { };
        radix = toInt32(*written);
    }
    Vector<char16_t> buffer;
    return PrimitiveKnownToFolding::number(parseInt(walker.types().viewOf(known[0].asText(), buffer), radix));
}

std::optional<PrimitiveKnownToFolding> foldExactly(Walker& walker, const CallByOneRow& call)
{
    if (!call.codeInLine || call.receiver)
        return std::nullopt;
    InlineBuiltin builtin = *call.codeInLine;

    if (std::optional<TestAskedInFolds> asked = testOfNumberAskedInFolds(builtin)) {
        if (call.arguments.empty())
            return std::nullopt;
        Reference argument = call.arguments[0];
        if (PrimitiveKnownToFolding known = primitiveKnownInNumberForms(walker, argument)) {
            if (!asked->converts && known.kind() != PrimitiveKnownToFolding::Kind::Number)
                return PrimitiveKnownToFolding::boolean(false);
            if (std::optional<double> number = numberOfKnownPrimitiveInFolds(walker, known))
                return PrimitiveKnownToFolding::boolean(numberPassesTestInFolds(asked->test, *number));
            return std::nullopt;
        }
        TypeID type = argument && argument.isRecord() ? walker.typeOf(argument) : TypeID();
        std::optional<bool> answer = type ? answerOfTestByTypeInFolds(*asked, walker.types().at(type)) : std::nullopt;
        if (!answer)
            return std::nullopt;
        return PrimitiveKnownToFolding::boolean(*answer);
    }

    Vector<PrimitiveKnownToFolding, 4> known;
    for (Reference argument : call.arguments) {
        known.append(primitiveKnownInNumberForms(walker, argument));
        if (!known.last())
            return std::nullopt;
    }

    PrimitiveKnownToFolding folded;
    if (builtin == InlineBuiltin::ParseInt)
        folded = foldParseIntInFolds(walker, known.span());
    else {
        Vector<double, 4> numbers;
        for (PrimitiveKnownToFolding argument : known) {
            // The row has converted what it converts. Whatever else is no number is not for here.
            if (argument.kind() != PrimitiveKnownToFolding::Kind::Number)
                return std::nullopt;
            numbers.append(argument.asNumber());
        }
        folded = foldCallOnKnownNumbersInFolds(builtin, numbers.span());
    }
    if (!folded)
        return std::nullopt;
    return folded;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
