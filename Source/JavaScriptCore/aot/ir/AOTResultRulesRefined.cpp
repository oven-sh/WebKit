/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResultRulesInternal.h"

#if ENABLE(AOT)

#include "AOTBody.h"
#include "AOTCode.h"
#include "AOTIRProgram.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTTypeNarrowing.h"
#include "AOTVector.h"
#include "PageCount.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static_assert([] {
    bool allHaveNaturalTypes = true;
    opcodesRefinedBeyondFixedNatural.forEach([&](Opcode opcode) {
        allHaveNaturalTypes &= resultRuleOf(opcode) == ResultRule::Natural;
    });
    return allHaveNaturalTypes;
}());

static bool hasTruthyAndFalsyPartsAtAGlance(const TypeArena& types, TypeID operand)
{
    constexpr uint16_t keptInFalsyPart = Primitive::UndefinedOrNull | Primitive::False;
    if (holdsOnlyPrimitives(operand))
        return primitivesInID(operand) & Primitive::True && primitivesInID(operand) & keptInFalsyPart;
    const Type& type = types.recordOf(operand);
    return type.isJavaScriptValue() && (type.hasObjects() || type.primitives & Primitive::True) && type.primitives & keptInFalsyPart;
}

static ALWAYS_INLINE TypeID refinedTypeOfToBoolean(TypeArena& types, TypeID operand, TypeID upperBound)
{
    if (hasTruthyAndFalsyPartsAtAGlance(types, operand))
        return upperBound;
    if (isNeverType(truthyPartOf(types, operand)))
        return TypeArena::fromPrimitives(Primitive::False);
    if (isNeverType(falsyPartOf(types, operand)))
        return TypeArena::fromPrimitives(Primitive::True);
    return upperBound;
}

static ALWAYS_INLINE TypeID refinedTypeOfStrictEquality(TypeArena& types, TypeID left, TypeID right, TypeID upperBound)
{
    Type leftType = types.at(left);
    Type rightType = types.at(right);
    auto mayBeZero = [](const Type& type) {
        return type.primitives & Primitive::WholeNumber || type.integers.contains(0);
    };
    bool mayBeEqual = types.overlap(left, right) || (mayBeZero(leftType) && mayBeZero(rightType));
    if (!mayBeEqual)
        return TypeArena::fromPrimitives(Primitive::False);
    if (left == right && isSingleValue(types, leftType) && !leftType.mayBeNaN())
        return TypeArena::fromPrimitives(Primitive::True);
    return upperBound;
}

static ALWAYS_INLINE bool typeTestSurelyPassesInResultRules(const TypingContext& context, TypeID value, TypeID tested)
{
    TypeArena& types = context.types;
    if (!types.isSubset(value, tested) || mayRefuseUntestableBuiltins(context, value, tested))
        return false;

    LayoutSetID testedLayouts = types.at(tested).unnamedLayouts;
    if (!testedLayouts || !types.at(value).hasObjects())
        return true;
    TypeID objectsOfTestedLayouts = types.forLayoutSet(testedLayouts);
    return types.testablePartOf(objectsOfTestedLayouts) == objectsOfTestedLayouts;
}

static ALWAYS_INLINE TypeID refinedTypeOfTypeTest(const TypingContext& context, TypeID value, TypeID tested, TypeID upperBound)
{
    if (typeTestSurelyPassesInResultRules(context, value, tested))
        return TypeArena::fromPrimitives(Primitive::True);
    if (!context.types.overlap(value, tested))
        return TypeArena::fromPrimitives(Primitive::False);
    return upperBound;
}

static TypeID refinedTypeOfQuotient(TypeArena& types, TypeID dividendID, TypeID divisorID, QuotientRounding rounding, TypeID upperBound)
{
    Type dividend = types.at(dividendID);
    Type divisor = types.at(divisorID);
    if (!dividend.isInteger() || !divisor.isInteger())
        return upperBound;
    TypeID quotient = integerQuotientType(types, dividend.integers, divisor.integers, rounding);
    return quotient ? quotient : upperBound;
}

static TypeID refinedTypeOfIntegerBound(TypeArena& types, TypeID numberID, TypeID upperBound)
{
    Type number = types.at(numberID);
    return number.isNumberWithinBounds() ? types.integer(number.boundsOfNumbers()) : upperBound;
}

static TypeID refinedTypeOfWholeRemainder(TypeArena& types, TypeID dividendID, TypeID divisorID, TypeID upperBound)
{
    Type dividend = types.at(dividendID);
    Type divisor = types.at(divisorID);
    if (!dividend.isInteger() || !divisor.isInteger())
        return upperBound;
    return wholeRemainderType(types, dividend.integers, divisor.integers);
}

static TypeID refinedTypeOfIntegerProduct(TypeArena& types, TypeID multiplicandID, TypeID multiplierID, TypeID upperBound)
{
    Type multiplicand = types.at(multiplicandID);
    Type multiplier = types.at(multiplierID);
    if (!multiplicand.isInteger() || !multiplier.isInteger())
        return upperBound;
    TypeID product = wholeProductType(types, multiplicand.integers, multiplier.integers);
    return product ? product : upperBound;
}

static TypeID refinedTypeOfProductSum(TypeArena& types, OperandTypes operands, TypeID upperBound)
{
    using namespace OperandIndex::MultiplyAddIntegers;
    for (TypeID operand : { operands[multiplicand], operands[multiplier], operands[addend] }) {
        if (!types.at(operand).isInteger())
            return upperBound;
    }
    std::optional<Range> sums = rangeOfProduct(types.at(operands[multiplicand]).integers, types.at(operands[multiplier]).integers);
    for (TypeID term : { operands[addend], operands[carry] }) {
        if (!sums || !sums->isSubsetOf(Range::full()) || (term && !types.at(term).isInteger()))
            return upperBound;
        if (term)
            sums = Range { sums->minimum + types.at(term).integers.minimum, sums->maximum + types.at(term).integers.maximum };
    }
    return sums->isSubsetOf(Range::full()) ? types.integer(*sums) : upperBound;
}

static TypeID refinedTypeOfStringLength(const TypingContext& context, TypeID stringID, TypeID upperBound)
{
    Type string = context.at(stringID);
    if (!string.hasOnlyPrimitives(Primitive::LiteralString))
        return upperBound;

    Vector<char16_t> buffer;
    Range lengths = Range::empty();
    context.types.at(string.stringLiterals).forEach([&](StringID literal) {
        lengths = lengths.unionWith(Range::one(context.program.strings.viewOf(literal, buffer).length()));
    });
    return lengths.isEmpty() ? upperBound : context.types.integer(lengths);
}

static TypeID refinedTypeOfSuperBase(const TypingContext& context, HomeObjectKind homeKind, TypeID home, TypeID upperBound)
{
    if (context.level != TypeLevel::Proven)
        return upperBound;
    Type type = context.at(home);
    FunctionID constructor = type.isObject() ? singleFunctionOf(context.types, type) : FunctionID();
    LayoutID classLayout = constructor && isClassConstructorOf(context.program.at(constructor).kind) ? context.program.at(constructor).constructedClass : LayoutID();
    if (!classLayout || context.program.at(classLayout).constructor != constructor)
        return upperBound;
    LayoutID above = context.positions.classAbove(classLayout);
    if (!above)
        return upperBound;
    switch (homeKind) {
    case HomeObjectKind::Object:
        return context.program.at(above).constructor ? context.types.forFunction(context.program.at(above).constructor) : upperBound;
    case HomeObjectKind::ClassPrototype:
        return context.types.forObject(context.types.objects().prototypeOf(above));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool holdsViewsOfNumbersAlone(const TypingContext& context, TypeID view)
{
    if (!view || holdsOnlyPrimitives(view))
        return false;
    Type type = context.types.at(view);
    if (!type.isObject() || !type.hasOnlyClassObjects())
        return false;
    const BuiltinTable& builtins = context.program.builtins();
    return std::ranges::all_of(classPartsOf(context.types, type), [&](const ClassPart& part) {
        TypedArrayType kind = builtins.typedArrayTypeOf(part.builtinClass);
        return isTypedView(kind) && !isBigInt(kind);
    });
}

template<typename Operation>
static TypeID refinedTypeOfTypedArrayElement(const TypingContext& context, const Operation& read, TypeID view, TypeID upperBound)
{
    RELEASE_ASSERT(context.level == TypeLevel::Expected || !testsItsOwnGuess(read));
    if (!isTypedView(read.typedArrayType()) && !holdsViewsOfNumbersAlone(context, view))
        return upperBound;
    TypeID element = isTypedView(read.typedArrayType()) ? elementTypeOf(read.typedArrayType()) : TypeID(TypeTable::number);
    return read.isInsideView() ? element : context.types.withPrimitives(element, Primitive::Undefined);
}

static_assert(maxTypedArrayLength == MAX_ARRAY_BUFFER_SIZE);

static TypeID refinedTypeOfTypedArrayLength(TypeArena& types, TypedArrayType type, TypeID upperBound)
{
    if (!isTypedView(type))
        return upperBound;
    return types.integer({ 0, static_cast<int64_t>(MAX_ARRAY_BUFFER_SIZE >> logElementSize(type)) });
}

static TypeID refinedTypeOfMirroredLength(TypeArena& types, TypedArrayType type, TypeID upperBound)
{
    if (!isTypedView(type))
        return upperBound;
    return types.integer({ 0, (1ll << 31) >> logElementSize(type) });
}

static TypeID refinedTypeOfConversion(TypeArena& types, Opcode opcode, TypeID operandID, TypeID upperBound)
{
    Type operand = types.at(operandID);
    switch (opcode) {
    case Opcode::ToString:
        return operand.isAnyString() ? operandID : upperBound;
    case Opcode::ToPropertyKey:
        if (operand.hasOnlyPrimitives(Primitive::AnyString | Primitive::AnySymbol))
            return operandID;
        if (operand.hasOnlyPrimitives(Primitive::NumberLike | Primitive::BigInt | Primitive::AnyString))
            return TypeTable::string;
        return upperBound;
    case Opcode::ToInt32:
        return operand.isInt32() ? operandID : upperBound;
    case Opcode::ToUint32:
        return operand.isInteger() && operand.integers.isSubsetOf(Range::uint32()) ? operandID : upperBound;
    case Opcode::ToIntegerOrInfinity:
        return operand.isInteger() ? operandID : TypeArena::fromPrimitives(Primitive::NumberWithoutFraction);
    case Opcode::ToLength: {
        constexpr Range lengths { 0, maxExactInteger - 1 };
        if (!operand.isInteger())
            return types.integer(lengths);
        return types.integer({ std::clamp(operand.integers.minimum, lengths.minimum, lengths.maximum), std::clamp(operand.integers.maximum, lengths.minimum, lengths.maximum) });
    }
    case Opcode::LogicalNot:
        if (operand.hasOnlyPrimitives(Primitive::True))
            return TypeArena::fromPrimitives(Primitive::False);
        if (operand.hasOnlyPrimitives(Primitive::False))
            return TypeArena::fromPrimitives(Primitive::True);
        return upperBound;
    case Opcode::ToObject:
        if (!operand.isJavaScriptValue() || operand.primitives & ~Primitive::UndefinedOrNull)
            return upperBound;
        return types.withOnlyObjects(operandID);
    case Opcode::ToThis:
        return operand.isObject() ? operandID : upperBound;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

template<typename Operation>
static TypeID refinedTypeOfOperation(const TypingContext& context, const Operation& operation, OperandTypes operands, TypeID upperBound)
{
    TypeArena& types = context.types;
    Opcode opcode = opcodeOf(operation);
    switch (opcode) {
    case Opcode::ToNumber:
    case Opcode::URShift: {
        if (opcode == Opcode::ToNumber && types.at(operands[OperandIndex::ToNumber::value]).isNumber())
            return operands[OperandIndex::ToNumber::value];
        TypeID number = numberResultOf(types, opcode, operands);
        return number ? number : upperBound;
    }
    case Opcode::ToBoolean:
        return refinedTypeOfToBoolean(types, operands[OperandIndex::ToBoolean::value], upperBound);
    case Opcode::StrictEq:
        return refinedTypeOfStrictEquality(types, operands[OperandIndex::StrictEq::left], operands[OperandIndex::StrictEq::right], upperBound);
    case Opcode::TypeTest:
        return refinedTypeOfTypeTest(context, operands[OperandIndex::TypeTest::value], operation.testedType(), upperBound);
    case Opcode::DivideIntegers:
        return refinedTypeOfQuotient(types, operands[OperandIndex::DivideIntegers::dividend], operands[OperandIndex::DivideIntegers::divisor], operation.rounding(), upperBound);
    case Opcode::RemainderOfIntegers:
        return refinedTypeOfWholeRemainder(types, operands[OperandIndex::RemainderOfIntegers::dividend], operands[OperandIndex::RemainderOfIntegers::divisor], upperBound);
    case Opcode::MultiplyIntegers:
        return refinedTypeOfIntegerProduct(types, operands[OperandIndex::MultiplyIntegers::multiplicand], operands[OperandIndex::MultiplyIntegers::multiplier], upperBound);
    case Opcode::MultiplyAddIntegers:
        return refinedTypeOfProductSum(types, operands, upperBound);
    case Opcode::RemainderByReciprocal:
        return refinedTypeOfWholeRemainder(types, operands[OperandIndex::RemainderByReciprocal::dividend], operands[OperandIndex::RemainderByReciprocal::divisor], upperBound);
    case Opcode::WholeDivisor:
        return types.integer(Range { 0, maxExactInteger });
    case Opcode::RemainderByWholeDivisor: {
        TypeID number = numberResultOf(types, Opcode::Mod, operands);
        return number ? number : upperBound;
    }
    case Opcode::IntegerBound:
        return refinedTypeOfIntegerBound(types, operands[OperandIndex::IntegerBound::number], upperBound);
    case Opcode::GetStringLength:
        return refinedTypeOfStringLength(context, operands[OperandIndex::GetStringLength::string], upperBound);
    case Opcode::GetTypedArrayElement:
        return refinedTypeOfTypedArrayElement(context, operation, operands[OperandIndex::GetTypedArrayElement::view], upperBound);
    case Opcode::GetDataViewElement:
        return elementTypeOf(operation.typedArrayType());
    case Opcode::GetMirroredLength:
        return refinedTypeOfMirroredLength(types, operation.typedArrayType(), upperBound);
    case Opcode::GetTypedArrayLength:
        return refinedTypeOfTypedArrayLength(types, operation.typedArrayType(), upperBound);
    case Opcode::GetSuperBase:
        return refinedTypeOfSuperBase(context, operation.homeKind(), operands[OperandIndex::GetSuperBase::home], upperBound);
    default:
        return refinedTypeOfConversion(types, opcode, operands[declaredPosition(OperandPosition::value(opcode))], upperBound);
    }
}

static TypeID refinedTypeWithin(TypeArena& types, TypeID refined, TypeID upperBound)
{
    if (refined == upperBound || types.isSubset(refined, upperBound))
        return refined;
    TypeID shared = types.intersectionOf(refined, upperBound);
    return types.isSubset(shared, upperBound) ? shared : upperBound;
}

template<typename Operation>
ALWAYS_INLINE_INTO_OTHER_FILES TypeID refineFixedNatural(const TypingContext& context, const Operation& operation, OperandTypes operands, TypeID upperBound)
{
    Opcode opcode = opcodeOf(operation);
    if (!opcodesRefinedBeyondFixedNatural.contains(opcode))
        return upperBound;

    uint32_t numberOfNamedOperands = numberOfNamedOperandsOf(opcode);
    for (uint32_t operandIndex = 0; operandIndex < numberOfNamedOperands; ++operandIndex) {
        TypeID operand = operands[operandIndex];
        RELEASE_ASSERT(operand || operandInfoOf(opcode, operandIndex).isOptional);
        if (isNeverType(operand))
            return TypeTable::never;
    }
    return refinedTypeWithin(context.types, refinedTypeOfOperation(context, operation, operands, upperBound), upperBound);
}

template TypeID refineFixedNatural<Draft>(const TypingContext&, const Draft&, OperandTypes, TypeID);
template TypeID refineFixedNatural<Operation>(const TypingContext&, const Operation&, OperandTypes, TypeID);
template TypeID refineFixedNatural<BodyValue>(const TypingContext&, const BodyValue&, OperandTypes, TypeID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
