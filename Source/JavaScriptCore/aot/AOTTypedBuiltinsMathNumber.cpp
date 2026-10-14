/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTRuntime.h"
#include "JSCInlines.h"
#include "JSGlobalObjectFunctions.h"
#include "MathCommon.h"
#include "NumberPrototype.h"
#include "ParseInt.h"
#include <wtf/WeakRandom.h>

namespace JSC { namespace AOT {

// The radix is from 2 to 36.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedNumberToStringWithRadix, JSCell*, (Instance* instance, double value, int32_t radix))
{
    VM& vm = *instance->vm;
    if (int32_t integer = truncateDoubleToInt32(value); integer == value)
        return int32ToString(vm, integer, radix);
    return numberToString(vm, value, radix);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedInt32ToStringWithRadix, JSCell*, (Instance* instance, int32_t value, int32_t radix))
{
    return int32ToString(*instance->vm, value, radix);
}

// As the functions of MathObject.cpp have them.
#define AOT_TYPED_DEFINE_MATH_FUNCTION(capitalizedName, lowerName) \
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedMath ## capitalizedName, double, (double x)) \
{ \
    return Math::lowerName(x); \
}
FOR_EACH_ARITH_UNARY_OP(AOT_TYPED_DEFINE_MATH_FUNCTION)
#undef AOT_TYPED_DEFINE_MATH_FUNCTION

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedMathATan2, double, (double y, double x))
{
    return atan2(y, x);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedMathHypot, double, (double x, double y))
{
    if (std::isinf(x) || std::isinf(y))
        return std::numeric_limits<double>::infinity();
    return std::hypot(x, y);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedMathHypot3, double, (double x, double y, double z))
{
    if (std::isinf(x) || std::isinf(y) || std::isinf(z))
        return std::numeric_limits<double>::infinity();
#if defined(_LIBCPP_VERSION) && _LIBCPP_VERSION < 200100
    return std::hypotl(std::hypotl(x, y), z);
#else
    if (std::isnan(x) || std::isnan(y) || std::isnan(z))
        return PNaN;
    return std::hypot(x, y, z);
#endif
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedMathF16Round, double, (double x))
{
    return Float16 { x };
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedParseInt8, double, (Instance*, const Latin1Character* characters, uint32_t length, int32_t radix))
{
    return parseInt(unsafeMakeSpan(characters, length), radix);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedStringToNumber8, double, (Instance*, const Latin1Character* characters, uint32_t length))
{
    return jsToNumber(StringView { unsafeMakeSpan(characters, length) });
}

namespace Typed {

using namespace B3;

template<> struct ParameterOfOperation<const Latin1Character*> { using Type = Pointer; };

// The family aot/builtins/math-number.def.

std::optional<Writes> writesOfInlineMathNumber(InlineBuiltin which, std::span<const Type> arguments)
{
    switch (which) {
    case InlineBuiltin::MathAbs:
    case InlineBuiltin::MathFloor:
    case InlineBuiltin::MathCeil:
    case InlineBuiltin::MathTrunc:
    case InlineBuiltin::MathRound:
    case InlineBuiltin::MathFround:
    case InlineBuiltin::MathF16Round:
    case InlineBuiltin::MathSqrt:
    case InlineBuiltin::MathClz32:
    case InlineBuiltin::MathSign:
    case InlineBuiltin::MathFunction:
        return writesIf(arguments.size() == 1 && areNumbers(arguments), Writes::Pure);
    case InlineBuiltin::NumberToString:
        return writesIf(arguments.empty() || (arguments[0].isInteger() && arguments[0].integers.isWithin(Range::int32())), Writes::RuntimeState);
    case InlineBuiltin::MathPow:
    case InlineBuiltin::MathImul:
    case InlineBuiltin::MathATan2:
        return writesIf(arguments.size() == 2 && areNumbers(arguments), Writes::Pure);
    case InlineBuiltin::MathHypot:
        return writesIf(arguments.size() <= 3 && areNumbers(arguments), Writes::Pure);
    case InlineBuiltin::MathMin:
    case InlineBuiltin::MathMax:
        return writesIf(std::ranges::all_of(arguments, isNumericOperand), Writes::Pure);
    case InlineBuiltin::MathRandom:
        return Writes::RuntimeState;
    case InlineBuiltin::IsNaN:
    case InlineBuiltin::IsFinite:
    case InlineBuiltin::NumberIsNaN:
    case InlineBuiltin::NumberIsFinite:
    case InlineBuiltin::NumberIsInteger:
    case InlineBuiltin::NumberIsSafeInteger: {
        if (arguments.size() != 1)
            return std::nullopt;
        bool converts = which == InlineBuiltin::IsNaN || which == InlineBuiltin::IsFinite;
        bool asksForNaN = which == InlineBuiltin::IsNaN || which == InlineBuiltin::NumberIsNaN;
        if (!converts && !(arguments[0].primitives & Primitive::Number))
            return Writes::Pure;
        switch (repOf(arguments[0])) {
        case Rep::Int64:
        case Rep::Double:
            return Writes::Pure;
        case Rep::OptionalInt64:
            return writesIf(!converts, Writes::Pure);
        // What stands for `undefined` and `null` among doubles is not a number, so it is not finite either. Only to the
        // question whether something is the number that is not one does it give the wrong answer.
        case Rep::OptionalDouble:
            return writesIf(!converts && !asksForNaN, Writes::Pure);
        default:
            return std::nullopt;
        }
    }
    // Looking at the characters flattens a rope.
    case InlineBuiltin::ToNumber:
        if (arguments.size() == 1 && arguments[0].isString())
            return Writes::RuntimeState;
        return writesIf(arguments.size() <= 1 && areNumbers(arguments), Writes::Pure);
    case InlineBuiltin::ParseInt:
        return writesIf(!arguments.empty() && arguments.size() <= 2 && arguments[0].isString() && (arguments.size() == 1 || (arguments[1].isInteger() && arguments[1].integers.isWithin(Range::int32()))), Writes::RuntimeState);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

// operationMathPow() with an exponent that is known.
// To a comparison with a number `undefined` is what NaN is. `null` is 0.
static bool isOptionalIntegerWithoutNull(const Program& program, const Value& value)
{
    return value.rep == Rep::OptionalInt64 && !(program.at(value.type).primitives & Primitive::Null);
}

std::optional<int64_t> Lowering::leastIfIntegerOrNaN(ValueID id) const
{
    auto& value = m_function.at(id);
    auto leastOf = [&](const Value& integer) -> std::optional<int64_t> {
        Range range = m_program.at(integer.type).integers;
        if (range.isEmpty())
            return std::nullopt;
        return range.least;
    };
    if (value.rep == Rep::Int64 || isOptionalIntegerWithoutNull(m_program, value))
        return leastOf(value);
    if (value.rep != Rep::Double && !(value.rep == Rep::OptionalDouble && value.opcode == Opcode::Parameter))
        return std::nullopt;
    switch (value.opcode) {
    case Opcode::Parameter: {
        auto found = m_leastOfNumbersJoinedWithIntegers.find(id.index());
        if (found == m_leastOfNumbersJoinedWithIntegers.end())
            return std::nullopt;
        return found->value;
    }
    // `undefined` becomes what is not a number.
    case Opcode::Cast: {
        auto& from = m_function.at(value.child(0));
        if (from.rep == Rep::Int64 || (from.rep == Rep::OptionalInt64 && !(m_program.at(from.type).primitives & Primitive::Null)))
            return leastOf(from);
        return std::nullopt;
    }
    case Opcode::CallBuiltin: {
        auto& implementation = m_builtins.at(value.builtin()).implementation;
        if (implementation.kind == BuiltinImplementation::Inline && implementation.inlined == InlineBuiltin::StringCharCodeAt)
            return 0;
        return std::nullopt;
    }
    default:
        return std::nullopt;
    }
}

Integer Lowering::integerOrMarker(ValueID id)
{
    if (m_function.at(id).rep == Rep::Int64)
        return in<Integer>(id);
    if (auto found = m_integersOfNumbers.find(id.index()); found != m_integersOfNumbers.end() && found->value.least && get(id).value == found->value.number)
        return Integer(found->value.integer);
    if (Operand operand = get(id); operand.rep == Rep::OptionalInt64)
        return select(isMarker(operand), integer(INT64_MIN), Integer(operand.value));
    Number number = in<Number>(id);
    return select(equal(number, number), truncateToInteger(number), integer(INT64_MIN));
}

// What is not a number is neither less nor more than anything: INT64_MIN is below every constant as a signed integer, and, counted from the least integer, above every one as an unsigned integer.
std::optional<Word32> Lowering::compareWithIntegerConstant(const Value& value)
{
    for (bool isNumberFirst : { true, false }) {
        ValueID number = value.child(isNumberFirst ? 0 : 1);
        auto& constant = m_function.at(value.child(isNumberFirst ? 1 : 0));
        if (constant.opcode != Opcode::Integer)
            continue;
        auto found = m_integersOfNumbers.find(number.index());
        // Where a test has shown more of the value it stands for something else.
        bool isKept = found != m_integersOfNumbers.end() && found->value.least && get(number).value == found->value.number;
        if (!isKept && !(isOptionalIntegerWithoutNull(m_program, m_function.at(number)) && get(number).rep == Rep::OptionalInt64 && leastIfIntegerOrNaN(number)))
            continue;
        Integer x = integerOrMarker(number);
        int64_t least = isKept ? *found->value.least : *leastIfIntegerOrNaN(number);
        int64_t bound = constant.integer();
        bool asksWhetherLess = (value.opcode == Opcode::Less || value.opcode == Opcode::LessEq) == isNumberFirst;
        bool admitsEqual = value.opcode == Opcode::LessEq || value.opcode == Opcode::GreaterEq;
        if (!asksWhetherLess)
            return admitsEqual ? greaterThanOrEqual(x, integer(bound)) : greaterThan(x, integer(bound));
        if (admitsEqual ? bound < least : bound <= least)
            return boolean(false);
        Integer fromLeast = least ? sub(x, integer(least)) : x;
        Integer limit = integer(bound - least);
        return admitsEqual ? belowOrEqual(fromLeast, limit) : below(fromLeast, limit);
    }
    return std::nullopt;
}

void Lowering::findNumbersJoinedWithIntegers(const Vector<BlockID>& order)
{
    UncheckedKeyHashMap<uint32_t, Vector<ValueID, 4>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> passedTo;
    for (auto& block : m_function.blocks) {
        for (auto& target : block.terminator.targets) {
            auto& parameters = m_function.at(target.block).parameters;
            for (unsigned i = 0; i < target.arguments.size() && i < parameters.size(); ++i) {
                auto& parameter = m_function.at(parameters[i]);
                if (parameter.rep == Rep::Double || (parameter.rep == Rep::OptionalDouble && !(m_program.at(parameter.type).primitives & Primitive::Null)))
                    passedTo.add(parameters[i].index(), Vector<ValueID, 4> { }).iterator->value.append(target.arguments[i]);
            }
        }
    }
    if (passedTo.isEmpty())
        return;
    // A parameter comes behind those that are passed to it, but around a loop.
    for (BlockID block : order) {
        if (!block.index())
            continue;
        for (ValueID id : m_function.at(block).parameters) {
            auto found = passedTo.find(id.index());
            if (found == passedTo.end())
                continue;
            std::optional<int64_t> least;
            bool areAllIntegersOrNaN = true;
            for (ValueID argument : found->value) {
                if (argument == id)
                    continue;
                auto leastOfArgument = leastIfIntegerOrNaN(argument);
                if (!leastOfArgument) {
                    areAllIntegersOrNaN = false;
                    break;
                }
                least = least ? std::min(*least, *leastOfArgument) : *leastOfArgument;
            }
            if (areAllIntegersOrNaN && least)
                m_leastOfNumbersJoinedWithIntegers.add(id.index(), *least);
        }
    }
}

void Lowering::findNumbersJoinedWithInt32(const Vector<BlockID>& order)
{
    auto note = [&](ValueID operand) {
        if (m_function.at(operand).opcode == Opcode::Parameter && m_function.at(operand).rep == Rep::Double)
            m_hasInt32Phi.set(operand.index());
    };
    for (auto& block : m_function.blocks) {
        for (ValueID id : block.values) {
            auto& value = m_function.at(id);
            switch (value.opcode) {
            case Opcode::BitAnd:
            case Opcode::BitOr:
            case Opcode::BitXor:
            case Opcode::LShift:
            case Opcode::RShift:
            case Opcode::URShift:
                note(value.child(0));
                note(value.child(1));
                break;
            case Opcode::BitNot:
                note(value.child(0));
                break;
            case Opcode::CallBuiltin:
                if (isInlineBuiltin(id, InlineBuiltin::MathImul) || isInlineBuiltin(id, InlineBuiltin::MathClz32)) {
                    for (ValueID argument : value.arguments)
                        note(argument);
                }
                break;
            default:
                break;
            }
        }
    }
    if (m_hasInt32Phi.isEmpty())
        return;
    for (ValueID id : m_function.blocks[0].parameters)
        m_hasInt32Phi.clear(id.index());
    Vector<unsigned> position;
    position.fill(0, m_function.blocks.size());
    for (unsigned i = 0; i < order.size(); ++i)
        position[order[i].index()] = i;
    BitVector isPassedInt32AroundLoop;
    for (BlockID from : order) {
        for (auto& target : m_function.at(from).terminator.targets) {
            if (position[from.index()] < position[target.block.index()])
                continue;
            auto& parameters = m_function.at(target.block).parameters;
            for (unsigned i = 0; i < target.arguments.size() && i < parameters.size(); ++i) {
                auto& passed = m_program.at(m_function.at(target.arguments[i]).type);
                if (passed.isInteger() && passed.integers.isWithin(Range::int32()))
                    isPassedInt32AroundLoop.set(parameters[i].index());
                else
                    m_hasInt32Phi.clear(parameters[i].index());
            }
        }
    }
    m_hasInt32Phi.filter(isPassedInt32AroundLoop);
}

std::optional<bool> Lowering::isMathMaxOrMin(ValueID id) const
{
    auto& definition = m_function.at(originOf(id));
    if (definition.opcode != Opcode::BuiltinFunction)
        return std::nullopt;
    auto& implementation = m_builtins.at(definition.builtin()).implementation;
    if (implementation.kind != BuiltinImplementation::Inline || (implementation.inlined != InlineBuiltin::MathMax && implementation.inlined != InlineBuiltin::MathMin))
        return std::nullopt;
    return implementation.inlined == InlineBuiltin::MathMax;
}

// Where an element is no number, or what comes out is not one: to the block. Nothing has been done by then.
Number Lowering::minOrMaxOfElements(bool isMax, CellPointer array, Pointer butterfly, Integer length, LBasicBlock otherwise)
{
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 2> results;
    auto fold = [&](IndexingType indexingType) {
        auto& elements = elementsOf(indexingType);
        LBasicBlock loop = m_out.newBlock();
        LBasicBlock element = m_out.newBlock();
        LBasicBlock done = m_out.newBlock();
        ValueFromBlock firstIndex = m_out.anchor(m_out.intPtrZero);
        ValueFromBlock nothingYet = m_out.anchor(m_out.constDouble(isMax ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity()));
        m_out.jump(loop);
        m_out.appendTo(loop);
        LValue index = m_out.phi(pointerType(), firstIndex);
        Number chosen(m_out.phi(Double, nothingYet));
        m_out.branch(m_out.below(index, length.value()), unsure(element), unsure(done));
        m_out.appendTo(element);
        TypedPointer address = m_out.baseIndex(elements, butterfly.value(), index, FTL::ScaleEight);
        Number next;
        if (indexingType == ArrayWithDouble)
            next = Number(m_out.loadDouble(address));
        else {
            Boxed boxed(m_out.load64(address));
            orElse(isNumber(boxed), otherwise);
            next = select(isInt32(boxed), toNumber(low32(boxed)), unboxedDouble(boxed));
        }
        m_out.addIncomingToPhi(index, m_out.anchor(m_out.add(index, m_out.intPtrOne)));
        m_out.addIncomingToPhi(chosen.value(), m_out.anchor((isMax ? max(chosen, next) : min(chosen, next)).value()));
        m_out.jump(loop);
        m_out.appendTo(done);
        results.append(m_out.anchor(chosen.value()));
        m_out.jump(continuation);
    };
    Word32 shape = indexingModeOf(array, IndexingShapeMask);
    LBasicBlock doubles = m_out.newBlock();
    LBasicBlock values = m_out.newBlock();
    m_out.branch(equal(shape, word32(DoubleShape)).value(), unsure(doubles), unsure(values));
    m_out.appendTo(doubles);
    fold(ArrayWithDouble);
    m_out.appendTo(values);
    orElse(logicalOr(equal(shape, word32(ContiguousShape)), equal(shape, word32(Int32Shape))), otherwise);
    fold(ArrayWithContiguous);
    m_out.appendTo(continuation);
    Number result(m_out.phi(Double, results));
    ensureStillAliveHere(array);
    orElse(equal(result, result), otherwise);
    return result;
}

std::optional<Number> Lowering::powerWithConstantExponent(Number base, ValueID exponent)
{
    ValueID constantID = m_program.constantOf(m_function, exponent);
    if (!constantID)
        return std::nullopt;
    auto& constant = m_function.at(constantID);
    double y;
    if (constant.opcode == Opcode::Integer)
        y = static_cast<double>(constant.integer());
    else if (constant.opcode == Opcode::Double)
        y = std::bit_cast<double>(constant.bits());
    else
        return std::nullopt;
    Number infinity = number(std::numeric_limits<double>::infinity());
    if (y == 0.5)
        return select(equal(abs(base), number(0)), number(0), select(equal(abs(base), infinity), infinity, sqrt(base)));
    if (y == -0.5)
        return select(equal(abs(base), number(0)), infinity, select(equal(abs(base), infinity), number(0), div(number(1), sqrt(base))));
    if (!(y >= 0 && y <= maxExponentForIntegerMathPow) || y != std::trunc(y))
        return std::nullopt;
    Number result;
    Number power = base;
    for (unsigned bits = static_cast<unsigned>(y); bits; bits >>= 1) {
        // One times a number is that number.
        if (bits & 1)
            result = result ? mul(result, power) : power;
        if (bits >> 1)
            power = mul(power, power);
    }
    return result ? result : number(1);
}

std::optional<Number> Lowering::powerWithConstantBase(ValueID base, ValueID exponent)
{
    auto& constant = m_function.at(originOf(base));
    double x;
    if (constant.opcode == Opcode::Integer)
        x = static_cast<double>(constant.integer());
    else if (constant.opcode == Opcode::Double)
        x = std::bit_cast<double>(constant.bits());
    else
        return std::nullopt;
    if (x != 2 && x != 10)
        return std::nullopt;
    Range handled = x == 2 ? Range { -1022, 1023 } : Range { 0, static_cast<int64_t>(AOT::powersOfTen.size()) - 1 };
    auto inPlace = [&](Integer k) {
        if (x == 2)
            return Number(m_out.bitCast(m_out.shl(add(k, integer(1023)).value(), m_out.constInt32(52)), Double));
        LValue table = fixedPointer(Instance::offsetOfPowersOfTen());
        return Number(m_out.loadDouble(TypedPointer(m_heaps.AOTInstance_fixedFields, m_out.add(table, m_out.shl(k.value(), m_out.constInt32(3))))));
    };
    auto& type = typeOf(exponent);
    if (type.isInteger() && type.integers.isWithin(handled))
        return inPlace(in<Integer>(exponent));

    LBasicBlock generally = newUnlikelyBlock();
    Join join(m_out, Rep::Double, Double);
    Number y = in<Number>(exponent);
    Integer k;
    if (type.isInteger())
        k = in<Integer>(exponent);
    else {
        Word32 narrow = low32(truncateToInteger(y));
        orElse(equal(toNumber(narrow), y), generally);
        k = signExtended(narrow);
    }
    orElse(belowOrEqual(sub(k, integer(handled.least)), integer(handled.most - handled.least)), generally);
    join.add(inPlace(k));
    m_out.appendTo(generally);
    join.add(Number(AOT_TYPED_OPERATION(Pow, number(x), y)));
    return join.finish().as<Number>();
}

static bool isExactInteger(const Type& type, int64_t limit = maxExactInteger)
{
    return type.isInteger() && type.integers.isWithin({ -limit, limit });
}

// Toward zero. The divisor is not zero.
Integer Lowering::divide(Integer x, Range dividends, Integer y, Range divisors)
{
    if (dividends.least >= 0 && divisors.least > 0 && dividends.isWithin(Range::uint32()) && divisors.isWithin(Range::uint32()))
        return zeroExtended(Word32(m_out.m_block->appendNew<B3::Value>(m_proc, UDiv, m_out.origin(), low32(x).value(), low32(y).value())));
    if (divisors.least == divisors.most && hasOneBitSet(divisors.least)) {
        unsigned shift = WTF::fastLog2(static_cast<uint64_t>(divisors.least));
        if (!shift)
            return x;
        if (dividends.least < 0)
            x = add(x, Integer(m_out.lShr(m_out.aShr(x.value(), m_out.constInt32(63)), m_out.constInt32(64 - shift))));
        return Integer(m_out.aShr(x.value(), m_out.constInt32(shift)));
    }
    if (dividends.isWithin(Range::int32()) && divisors.isWithin(Range::int32()) && (dividends.least > INT32_MIN || divisors.least > 0))
        return signExtended(Word32(m_out.div(low32(x).value(), low32(y).value())));
    return Integer(m_out.div(x.value(), y.value()));
}

// Math.floor, ceil, trunc or round of x / y. The double nearest to x / y is on the same side of every integer as x / y itself up to 2^53, and of every integer and a half up to 2^51.
std::optional<Integer> Lowering::roundedQuotient(InlineBuiltin which, ValueID argument, bool yieldsInteger)
{
    auto& quotient = m_function.at(m_program.originOf(m_function, argument));
    if (quotient.opcode != Opcode::Div)
        return std::nullopt;
    int64_t limit = which == InlineBuiltin::MathRound ? maxExactInteger >> 2 : maxExactInteger;
    if (!isExactInteger(typeOf(quotient.child(0)), limit) || !isExactInteger(typeOf(quotient.child(1)), limit))
        return std::nullopt;
    Range dividends = typeOf(quotient.child(0)).integers;
    Range divisors = typeOf(quotient.child(1)).integers;
    bool isDividendConstant = dividends.least == dividends.most;
    bool isDivisorConstant = divisors.least == divisors.most;
    if (divisors.contains(0) || (which == InlineBuiltin::MathRound && divisors.least < 0) || (isDividendConstant && isDivisorConstant))
        return std::nullopt;
    // A zero that comes out negative is no integer.
    bool isNeverNegative = dividends.least >= 0 && divisors.least > 0;
    if (!yieldsInteger && !isNeverNegative && !(which == InlineBuiltin::MathFloor && divisors.least > 0))
        return std::nullopt;

    Integer x = in<Integer>(quotient.child(0));
    Integer y = in<Integer>(quotient.child(1));
    if (which == InlineBuiltin::MathRound) {
        // A constant that the back end folds is none that integer() has seen.
        x = add(isDividendConstant ? integer(2 * dividends.least) : add(x, x), y);
        y = isDivisorConstant ? integer(2 * divisors.least) : add(y, y);
        dividends = { 2 * dividends.least + divisors.least, 2 * dividends.most + divisors.most };
        divisors = { 2 * divisors.least, 2 * divisors.most };
        which = InlineBuiltin::MathFloor;
    }
    bool roundsUp = which == InlineBuiltin::MathCeil;
    if (isDivisorConstant && hasOneBitSet(divisors.least) && which != InlineBuiltin::MathTrunc) {
        if (roundsUp)
            x = add(x, integer(divisors.least - 1));
        return Integer(m_out.aShr(x.value(), m_out.constInt32(WTF::fastLog2(static_cast<uint64_t>(divisors.least)))));
    }
    if (roundsUp && isNeverNegative) {
        if (isDivisorConstant)
            x = add(x, integer(divisors.least - 1));
        else if (isDividendConstant)
            x = add(y, integer(dividends.least - 1));
        else
            x = add(x, sub(y, integer(1)));
        dividends.most += divisors.most - 1;
    }
    Integer truncated = divide(x, dividends, y, divisors);
    if (which == InlineBuiltin::MathTrunc || isNeverNegative)
        return truncated;
    Integer remainder = sub(x, mul(truncated, y));
    Word32 isNegative = lessThan(bitXor(remainder, y), integer(0));
    Word32 isInexact = isNotZero(remainder);
    if (roundsUp)
        return add(truncated, zeroExtended(logicalAnd(isInexact, logicalNot(isNegative))));
    return sub(truncated, zeroExtended(logicalAnd(isInexact, isNegative)));
}

// A division by zero yields an infinity or what is not a number: 0, as of the chill division.
std::optional<Word32> Lowering::toInt32OfQuotient(ValueID id)
{
    auto& quotient = m_function.at(id);
    if (quotient.opcode != Opcode::Div || !isExactInteger(typeOf(quotient.child(0))) || !isExactInteger(typeOf(quotient.child(1))))
        return std::nullopt;
    Integer x = in<Integer>(quotient.child(0));
    Integer y = in<Integer>(quotient.child(1));
    Range dividends = typeOf(quotient.child(0)).integers;
    Range divisors = typeOf(quotient.child(1)).integers;
    if (!divisors.contains(0))
        return low32(divide(x, dividends, y, divisors));
    if (dividends.isWithin(Range::int32()) && divisors.isWithin(Range::int32()))
        return Word32(m_out.chillDiv(low32(x).value(), low32(y).value()));
    return low32(Integer(m_out.chillDiv(x.value(), y.value())));
}

std::optional<Operand> Lowering::lowerInlineMathNumber(InlineBuiltin which, ValueID result, const Value& value, Writes writes)
{
    auto& type = typeOf(result);
    auto& arguments = value.arguments;
    auto& builtin = m_builtins.at(value.builtin());
    auto isFinite = [&](Number x) {
        return lessThan(abs(x), number(std::numeric_limits<double>::infinity()));
    };
    auto isFiniteInteger = [&](Number x) {
        return logicalAnd(equal(trunc(x), x), isFinite(x));
    };
    auto areIntegers = [&] {
        for (ValueID argument : arguments) {
            if (!typeOf(argument).isInteger())
                return false;
        }
        return true;
    };
    // The characters of a string that is flat, or part of one that is, and 8-bit. Anything else is left to the engine's function.
    auto ofLatin1Characters = [&](ValueID string, auto&& functor) {
        Rep rep = repOf(type);
        LBasicBlock otherwise = newBlockForGeneralCall(value);
        Join join(m_out, rep, b3TypeOf(rep));
        Characters characters = charactersOf(stringReceiver(get(string).as<CellPointer>()), otherwise);
        orElse(hasBits(characters.flags, StringImpl::flagIs8Bit()), otherwise);
        Number yielded = functor(characters);
        ensureStillAliveHere(characters.owner);
        join.add(yieldNumber(yielded, type));
        if (!isGeneralArmOnOffer(otherwise)) {
            m_out.appendTo(otherwise);
            Boxed generally = callBuiltinFunction(value, OperationEffects { writes, true });
            join.add(vouchesForItsType(m_program, m_function, m_function.at(result)) ? convert(generally, type, rep) : ensure(generally, type));
        }
        return join.finish();
    };

    switch (which) {
    case InlineBuiltin::MathFloor:
    case InlineBuiltin::MathCeil:
    case InlineBuiltin::MathTrunc:
    case InlineBuiltin::MathRound:
        if (typeOf(arguments[0]).isInteger())
            return yieldValue(arguments[0], type);
        if (auto quotient = roundedQuotient(which, arguments[0], type.isInteger()))
            return type.isInteger() ? convert(*quotient, type, repOf(type)) : ensure(*quotient, Type::integer({ -maxExactInteger, maxExactInteger }), type);
        [[fallthrough]];
    case InlineBuiltin::MathAbs:
        if (which == InlineBuiltin::MathAbs && typeOf(arguments[0]).isInteger() && (type.isInteger() || repOf(type) == Rep::JSValue)) {
            Integer x = in<Integer>(arguments[0]);
            Integer sign(m_out.aShr(x.value(), m_out.constInt32(63)));
            Integer magnitude = sub(bitXor(x, sign), sign);
            if (type.isInteger())
                return convert(magnitude, type, repOf(type));
            Range range = typeOf(arguments[0]).integers;
            return ensure(magnitude, Type::integer({ 0, std::max(-range.least, range.most) }), type);
        }
        [[fallthrough]];
    case InlineBuiltin::MathFround:
    case InlineBuiltin::MathSqrt: {
        Number x = in<Number>(arguments[0]);
        switch (which) {
        case InlineBuiltin::MathAbs:
            return yieldNumber(abs(x), type);
        case InlineBuiltin::MathFloor:
            return yieldNumber(floor(x), type);
        case InlineBuiltin::MathCeil:
            return yieldNumber(ceil(x), type);
        case InlineBuiltin::MathTrunc:
            return yieldNumber(trunc(x), type);
        // Math::roundDouble().
        case InlineBuiltin::MathRound: {
            Number integer = ceil(x);
            return yieldNumber(select(greaterThan(sub(integer, number(0.5)), x), sub(integer, number(1)), integer), type);
        }
        case InlineBuiltin::MathFround:
            return yieldNumber(Number(m_out.fround(x.value())), type);
        default:
            return yieldNumber(sqrt(x), type);
        }
    }
    // What is neither above nor below zero is yielded as it is: either zero, and what is not a number.
    case InlineBuiltin::MathSign: {
        if (typeOf(arguments[0]).isInteger()) {
            Integer x = in<Integer>(arguments[0]);
            Integer isNegative(m_out.aShr(x.value(), m_out.constInt32(63)));
            Integer isPositive(m_out.lShr(neg(x).value(), m_out.constInt32(63)));
            return ensure(bitOr(isNegative, isPositive), Type::integer({ -1, 1 }), type);
        }
        Number x = in<Number>(arguments[0]);
        return yieldNumber(select(greaterThan(x, number(0)), number(1), select(lessThan(x, number(0)), number(-1), x)), type);
    }
    case InlineBuiltin::MathFunction: {
        Number x = in<Number>(arguments[0]);
#define AOT_TYPED_CALL_MATH_FUNCTION(capitalizedName, lowerName) \
        if (builtin.name == ASCIILiteral::fromLiteralUnsafe(#lowerName)) \
            return yieldNumber(AOT_TYPED_OPERATION(Math ## capitalizedName, x), type);
        FOR_EACH_ARITH_UNARY_OP(AOT_TYPED_CALL_MATH_FUNCTION)
#undef AOT_TYPED_CALL_MATH_FUNCTION
        RELEASE_ASSERT_NOT_REACHED();
    }
    case InlineBuiltin::MathATan2:
        return yieldNumber(AOT_TYPED_OPERATION(MathATan2, in<Number>(arguments[0]), in<Number>(arguments[1])), type);
    case InlineBuiltin::MathHypot:
        switch (arguments.size()) {
        case 0:
            return yieldNumber(number(0), type);
        case 1:
            return yieldNumber(abs(in<Number>(arguments[0])), type);
        case 2:
            return yieldNumber(AOT_TYPED_OPERATION(MathHypot, in<Number>(arguments[0]), in<Number>(arguments[1])), type);
        default:
            return yieldNumber(AOT_TYPED_OPERATION(MathHypot3, in<Number>(arguments[0]), in<Number>(arguments[1]), in<Number>(arguments[2])), type);
        }
    case InlineBuiltin::MathF16Round:
        return yieldNumber(AOT_TYPED_OPERATION(MathF16Round, in<Number>(arguments[0])), type);
    // WeakRandom::get().
    case InlineBuiltin::MathRandom: {
        LValue globalObject = fixedPointer(Instance::offsetOfGlobalObject());
        TypedPointer lowAddress = m_out.address(m_heaps.AOTTyped_runtimeState, globalObject, JSGlobalObject::weakRandomOffset() + WeakRandom::lowOffset());
        TypedPointer highAddress = m_out.address(m_heaps.AOTTyped_runtimeState, globalObject, JSGlobalObject::weakRandomOffset() + WeakRandom::highOffset());
        LValue x = m_out.load64(lowAddress);
        LValue y = m_out.load64(highAddress);
        m_out.store64(y, lowAddress);
        x = m_out.bitXor(x, m_out.shl(x, m_out.constInt32(23)));
        x = m_out.bitXor(x, m_out.lShr(x, m_out.constInt32(17)));
        x = m_out.bitXor(x, m_out.bitXor(y, m_out.lShr(y, m_out.constInt32(26))));
        m_out.store64(x, highAddress);
        didEmitWrite(Writes::RuntimeState);
        LValue bits = m_out.bitAnd(m_out.add(x, y), m_out.constInt64((1ULL << 53) - 1));
        return yieldNumber(mul(Number(m_out.intToDouble(bits)), number(1.0 / (1ULL << 53))), type);
    }
    case InlineBuiltin::NumberToString: {
        auto& stringType = m_program.at(m_program.types.string);
        auto& receiver = typeOf(value.child(0));
        auto yield = [&](CellPointer string) {
            // What the table says is a string is neither `undefined` nor `null`, whatever the type of the value admits. Not where this is one arm of a test on what is expected.
            if (isOnTrunk())
                m_passedNilTestIn.add(result.index(), Vector<BlockID, 1> { }).iterator->value.append(m_currentBlockID);
            return ensure(string, stringType, type);
        };
        auto generally = [&] {
            return asCell(callBuiltinFunction(value, OperationEffects { Writes::RuntimeState, true }));
        };
        // Of `undefined` and `null` the engine's function says what is wrong.
        if (!receiver.isNumber())
            return yield(generally());
        if (arguments.isEmpty())
            return yield(toStringCell(value.child(0)));
        Word32 radix = low32(in<Integer>(arguments[0]));
        auto withRadix = [&] {
            if (receiver.isInteger() && receiver.integers.isWithin(Range::int32()))
                return AOT_TYPED_OPERATION(Int32ToStringWithRadix, low32(in<Integer>(value.child(0))), radix);
            return AOT_TYPED_OPERATION(NumberToStringWithRadix, in<Number>(value.child(0)), radix);
        };
        if (typeOf(arguments[0]).integers.isWithin({ 2, 36 }))
            return yield(withRadix());
        return yield(either<CellPointer>(below(binary<Word32>(&Output::sub, radix, word32(2)), word32(35)), withRadix, generally));
    }
    case InlineBuiltin::MathPow: {
        Number base = in<Number>(arguments[0]);
        if (auto power = powerWithConstantExponent(base, arguments[1]))
            return yieldNumber(*power, type);
        if (auto power = powerWithConstantBase(arguments[0], arguments[1]))
            return yieldNumber(*power, type);
        return yieldNumber(AOT_TYPED_OPERATION(Pow, base, in<Number>(arguments[1])), type);
    }
    case InlineBuiltin::MathMin:
    case InlineBuiltin::MathMax: {
        bool isMax = which == InlineBuiltin::MathMax;
        if (arguments.isEmpty())
            return yieldNumber(number(isMax ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity()), type);
        // Of integers it is one of them. What range that leaves is the checker's to say (integers.rs, numbers_of_math()), as of a sum.
        if (areIntegers() && (type.isInteger() || repOf(type) == Rep::JSValue)) {
            Integer chosen = in<Integer>(arguments[0]);
            Range range = typeOf(arguments[0]).integers;
            for (ValueID argument : arguments.span().subspan(1)) {
                Integer next = in<Integer>(argument);
                chosen = select(isMax ? greaterThan(chosen, next) : lessThan(chosen, next), chosen, next);
                range = range.hull(typeOf(argument).integers);
            }
            return type.isInteger() ? convert(chosen, type, repOf(type)) : ensure(chosen, Type::integer(range), type);
        }
        Number chosen = asNumericOperand(arguments[0]);
        for (ValueID argument : arguments.span().subspan(1))
            chosen = isMax ? max(chosen, asNumericOperand(argument)) : min(chosen, asNumericOperand(argument));
        return yieldNumber(chosen, type);
    }
    case InlineBuiltin::MathImul:
        return ensure(signExtended(mul(toInt32(arguments[0]), toInt32(arguments[1]))), m_program.at(m_program.types.int32), type);
    case InlineBuiltin::MathClz32:
        return ensure(zeroExtended(countLeadingZeros(toInt32(arguments[0]))), Type::integer({ 0, 32 }), type);

    case InlineBuiltin::IsNaN:
    case InlineBuiltin::IsFinite:
    case InlineBuiltin::NumberIsNaN:
    case InlineBuiltin::NumberIsFinite:
    case InlineBuiltin::NumberIsInteger:
    case InlineBuiltin::NumberIsSafeInteger: {
        Range safeIntegers { -(maxExactInteger - 1), maxExactInteger - 1 };
        bool converts = which == InlineBuiltin::IsNaN || which == InlineBuiltin::IsFinite;
        bool asksForNaN = which == InlineBuiltin::IsNaN || which == InlineBuiltin::NumberIsNaN;
        if (!converts && !(typeOf(arguments[0]).primitives & Primitive::Number))
            return yieldBoolean(boolean(false), type);
        Operand operand = get(arguments[0]);
        if (operand.rep == Rep::Int64) {
            if (which == InlineBuiltin::NumberIsSafeInteger)
                return yieldBoolean(isWithin(operand.as<Integer>(), safeIntegers), type);
            return yieldBoolean(boolean(!asksForNaN), type);
        }
        if (operand.rep == Rep::OptionalInt64) {
            if (asksForNaN)
                return yieldBoolean(boolean(false), type);
            // No marker is within the range.
            if (which == InlineBuiltin::NumberIsSafeInteger)
                return yieldBoolean(isWithin(assumedInteger(operand.as<OptionalInteger>()), safeIntegers), type);
            return yieldBoolean(logicalNot(isMarker(operand)), type);
        }
        Number x = operand.rep == Rep::Double ? operand.as<Number>() : assumedNumber(operand.as<OptionalNumber>());
        switch (which) {
        case InlineBuiltin::IsNaN:
        case InlineBuiltin::NumberIsNaN:
            return yieldBoolean(notEqualOrUnordered(x, x), type);
        case InlineBuiltin::IsFinite:
        case InlineBuiltin::NumberIsFinite:
            return yieldBoolean(isFinite(x), type);
        case InlineBuiltin::NumberIsInteger:
            return yieldBoolean(isFiniteInteger(x), type);
        // What is within the range is finite.
        default:
            return yieldBoolean(logicalAnd(equal(trunc(x), x), lessThanOrEqual(abs(x), number(static_cast<double>(safeIntegers.most)))), type);
        }
    }
    case InlineBuiltin::ToNumber:
        if (arguments.isEmpty())
            return ensure(integer(0), Type::integer(Range::one(0)), type);
        if (typeOf(arguments[0]).isString()) {
            return ofLatin1Characters(arguments[0], [&](const Characters& characters) {
                return AOT_TYPED_OPERATION(StringToNumber8, characters.data, low32(characters.length));
            });
        }
        return yieldValue(arguments[0], type);
    // No radix is as good as zero.
    case InlineBuiltin::ParseInt:
        return ofLatin1Characters(arguments[0], [&](const Characters& characters) {
            return AOT_TYPED_OPERATION(ParseInt8, characters.data, low32(characters.length), arguments.size() > 1 ? low32(in<Integer>(arguments[1])) : word32(0));
        });
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
