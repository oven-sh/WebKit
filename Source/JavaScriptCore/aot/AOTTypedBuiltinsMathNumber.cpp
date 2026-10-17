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

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedParseInt16, double, (Instance*, uint64_t characters, uint32_t length, int32_t radix))
{
    return parseInt(unsafeMakeSpan(std::bit_cast<const char16_t*>(characters), length), radix);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedStringToNumber16, double, (Instance*, uint64_t characters, uint32_t length))
{
    return jsToNumber(StringView { unsafeMakeSpan(std::bit_cast<const char16_t*>(characters), length) });
}

namespace Typed {

using namespace B3;

template<> struct OperationParameterType<const Latin1Character*> { using Type = Pointer; };

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
        return writesIf(arguments.size() == 1 && isNumericOperand(arguments[0]), Writes::Pure);
    case InlineBuiltin::NumberToString:
        return writesIf(arguments.empty() || (arguments[0].isInteger() && arguments[0].integers.isSubsetOf(Range::int32())), Writes::RuntimeState);
    case InlineBuiltin::MathPow:
        return writesIf(arguments.size() == 2 && isNumericOperand(arguments[0]) && arguments[1].isNumber(), Writes::Pure);
    case InlineBuiltin::MathImul:
    case InlineBuiltin::MathATan2:
        return writesIf(arguments.size() == 2 && std::ranges::all_of(arguments, isNumericOperand), Writes::Pure);
    case InlineBuiltin::MathHypot:
        return writesIf(arguments.size() <= 3 && std::ranges::all_of(arguments, isNumericOperand), Writes::Pure);
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
        bool testsForNaN = which == InlineBuiltin::IsNaN || which == InlineBuiltin::NumberIsNaN;
        if (!converts && !(arguments[0].primitives & Primitive::Number))
            return Writes::Pure;
        switch (repOf(arguments[0])) {
        case Rep::Int64:
        case Rep::Double:
            return Writes::Pure;
        case Rep::OptionalInt64:
            return writesIf(!converts || isNumericOperand(arguments[0]), Writes::Pure);
        case Rep::OptionalDouble:
            return writesIf(!converts || isNumericOperand(arguments[0]), Writes::Pure);
        case Rep::JSValue:
            return writesIf(!converts, Writes::Pure);
        default:
            return std::nullopt;
        }
    }
    case InlineBuiltin::ToNumber:
        if (arguments.size() == 1 && arguments[0].isString())
            return Writes::RuntimeState;
        if (arguments.size() == 1 && !areNumbers(arguments))
            return arguments[0].hasObjects() ? Writes::Heap : Writes::RuntimeState;
        return writesIf(arguments.size() <= 1, Writes::Pure);
    case InlineBuiltin::ParseInt:
        return writesIf(!arguments.empty() && arguments.size() <= 2 && arguments[0].isString() && (arguments.size() == 1 || (arguments[1].isInteger() && arguments[1].integers.isSubsetOf(Range::int32()))), Writes::RuntimeState);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static bool isOptionalIntegerWithoutNull(const Program& program, const Value& value)
{
    return value.rep == Rep::OptionalInt64 && !(program.at(value.type).primitives & Primitive::Null);
}

std::optional<int64_t> Lowering::minIfIntegerOrNaN(ValueID id) const
{
    auto& value = m_function.at(id);
    auto minValueOf = [&](const Value& integer) -> std::optional<int64_t> {
        Range range = m_program.at(integer.type).integers;
        if (range.isEmpty())
            return std::nullopt;
        return range.minimum;
    };
    if (value.rep == Rep::Int64 || isOptionalIntegerWithoutNull(m_program, value))
        return minValueOf(value);
    if (value.rep != Rep::Double && !(value.rep == Rep::OptionalDouble && value.opcode == Opcode::Parameter))
        return std::nullopt;
    switch (value.opcode) {
    case Opcode::Parameter: {
        auto found = m_minOfNumbersJoinedWithIntegers.find(id.index());
        if (found == m_minOfNumbersJoinedWithIntegers.end())
            return std::nullopt;
        return found->value;
    }
    case Opcode::Cast: {
        auto& from = m_function.at(value.child(0));
        if (from.rep == Rep::Int64 || (from.rep == Rep::OptionalInt64 && !(m_program.at(from.type).primitives & Primitive::Null)))
            return minValueOf(from);
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
    if (auto found = m_numbersAsIntegers.find(id.index()); found != m_numbersAsIntegers.end() && found->value.minimum && get(id).value == found->value.number)
        return Integer(found->value.integer);
    if (Operand operand = get(id); operand.rep == Rep::OptionalInt64)
        return select(isMarker(operand), integer(INT64_MIN), Integer(operand.value));
    Number number = in<Number>(id);
    return select(equal(number, number), truncateToInteger(number), integer(INT64_MIN));
}

std::optional<Word32> Lowering::compareWithIntegerConstant(const Value& value)
{
    for (bool isNumberFirst : { true, false }) {
        ValueID number = value.child(isNumberFirst ? 0 : 1);
        auto& constant = m_function.at(value.child(isNumberFirst ? 1 : 0));
        if (constant.opcode != Opcode::Integer)
            continue;
        auto found = m_numbersAsIntegers.find(number.index());
        bool isKept = found != m_numbersAsIntegers.end() && found->value.minimum && get(number).value == found->value.number;
        if (!isKept && !(isOptionalIntegerWithoutNull(m_program, m_function.at(number)) && get(number).rep == Rep::OptionalInt64 && minIfIntegerOrNaN(number)))
            continue;
        Integer x = integerOrMarker(number);
        int64_t least = isKept ? *found->value.minimum : *minIfIntegerOrNaN(number);
        int64_t bound = constant.integer();
        bool testsLess = (value.opcode == Opcode::Less || value.opcode == Opcode::LessEq) == isNumberFirst;
        bool includesEqual = value.opcode == Opcode::LessEq || value.opcode == Opcode::GreaterEq;
        if (!testsLess)
            return includesEqual ? greaterThanOrEqual(x, integer(bound)) : greaterThan(x, integer(bound));
        if (includesEqual ? bound < least : bound <= least)
            return boolean(false);
        Integer fromMinimum = least ? sub(x, integer(least)) : x;
        Integer limit = integer(bound - least);
        return includesEqual ? belowOrEqual(fromMinimum, limit) : below(fromMinimum, limit);
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
    constexpr int64_t noValueYet = std::numeric_limits<int64_t>::max();
    for (auto& entry : passedTo)
        m_minOfNumbersJoinedWithIntegers.add(entry.key, noValueYet);
    bool changed = true;
    while (changed) {
        changed = false;
        for (BlockID block : order) {
            if (!block.index())
                continue;
            for (ValueID id : m_function.at(block).parameters) {
                auto known = m_minOfNumbersJoinedWithIntegers.find(id.index());
                if (known == m_minOfNumbersJoinedWithIntegers.end())
                    continue;
                int64_t least = known->value;
                bool areAllIntegersOrNaN = true;
                for (ValueID argument : passedTo.find(id.index())->value) {
                    if (argument == id)
                        continue;
                    auto argumentMin = minIfIntegerOrNaN(argument);
                    if (!argumentMin) {
                        areAllIntegersOrNaN = false;
                        break;
                    }
                    least = std::min(least, *argumentMin);
                }
                if (!areAllIntegersOrNaN) {
                    m_minOfNumbersJoinedWithIntegers.remove(known);
                    changed = true;
                } else if (least != known->value) {
                    known->value = least;
                    changed = true;
                }
            }
        }
    }
    m_minOfNumbersJoinedWithIntegers.removeIf([&](auto& entry) {
        return entry.value == noValueYet;
    });
}

void Lowering::findConstantJoinsUsedAsNumbers()
{
    auto& joins = m_isConstantJoinUsedAsNumber;
    for (unsigned i = 1; i < m_function.blocks.size(); ++i) {
        for (ValueID id : m_function.blocks[i].parameters) {
            if (m_function.at(id).rep == Rep::Int64)
                joins.set(id.index());
        }
    }
    if (joins.isEmpty())
        return;
    BitVector isUsed;
    BitVector receivesConstant;
    for (auto& block : m_function.blocks) {
        if (block.terminator.value)
            joins.clear(block.terminator.value.index());
        for (auto& target : block.terminator.targets) {
            auto& parameters = m_function.at(target.block).parameters;
            for (unsigned i = 0; i < target.arguments.size(); ++i) {
                joins.clear(target.arguments[i].index());
                if (i >= parameters.size())
                    continue;
                if (m_function.at(target.arguments[i]).opcode == Opcode::Integer)
                    receivesConstant.set(parameters[i].index());
                else
                    joins.clear(parameters[i].index());
            }
        }
    }
    joins.filter(receivesConstant);
    for (auto& block : m_function.blocks) {
        if (joins.isEmpty())
            return;
        for (ValueID id : block.values) {
            auto& value = m_function.at(id);
            bool acceptsNumbers = value.rep == Rep::Double && (value.opcode == Opcode::Add || value.opcode == Opcode::Sub || value.opcode == Opcode::Mul || value.opcode == Opcode::Div)
                && !(typeOf(value.child(0)).isInteger() && typeOf(value.child(1)).isInteger()) && isNumericOperand(typeOf(value.child(0))) && isNumericOperand(typeOf(value.child(1)));
            value.forEachChild([&](ValueID child) {
                if (!acceptsNumbers)
                    joins.clear(child.index());
                isUsed.set(child.index());
            });
        }
    }
    joins.filter(isUsed);
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
    BitVector carriesInt32AroundLoop;
    for (BlockID from : order) {
        for (auto& target : m_function.at(from).terminator.targets) {
            if (position[from.index()] < position[target.block.index()])
                continue;
            auto& parameters = m_function.at(target.block).parameters;
            for (unsigned i = 0; i < target.arguments.size() && i < parameters.size(); ++i) {
                auto& passed = m_program.at(m_function.at(target.arguments[i]).type);
                if (passed.isInteger() && passed.integers.isSubsetOf(Range::int32()))
                    carriesInt32AroundLoop.set(parameters[i].index());
                else
                    m_hasInt32Phi.clear(parameters[i].index());
            }
        }
    }
    m_hasInt32Phi.filter(carriesInt32AroundLoop);
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
        ValueFromBlock initialValue = m_out.anchor(m_out.constDouble(isMax ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity()));
        m_out.jump(loop);
        m_out.appendTo(loop);
        LValue index = m_out.phi(pointerType(), firstIndex);
        Number chosen(m_out.phi(Double, initialValue));
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
    if (y == 1)
        return purified(base);
    Number result;
    Number power = base;
    for (unsigned bits = static_cast<unsigned>(y); bits; bits >>= 1) {
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
    auto inlinePath = [&](Integer k) {
        if (x == 2)
            return Number(m_out.bitCast(m_out.shl(add(k, integer(1023)).value(), m_out.constInt32(52)), Double));
        LValue table = fixedPointer(Instance::offsetOfPowersOfTen());
        return Number(m_out.loadDouble(TypedPointer(m_heaps.AOTInstance_fixedFields, m_out.add(table, m_out.shl(k.value(), m_out.constInt32(3))))));
    };
    auto& type = typeOf(exponent);
    if (type.isInteger() && type.integers.isSubsetOf(handled))
        return inlinePath(in<Integer>(exponent));

    LBasicBlock genericPath = newUnlikelyBlock();
    Join join(m_out, Rep::Double, Double);
    Number y = asNumericOperand(exponent);
    Integer k;
    if (type.isInteger())
        k = in<Integer>(exponent);
    else {
        Word32 narrow = low32(truncateToInteger(y));
        orElse(equal(toNumber(narrow), y), genericPath);
        k = signExtended(narrow);
    }
    orElse(belowOrEqual(sub(k, integer(handled.minimum)), integer(handled.maximum - handled.minimum)), genericPath);
    join.add(inlinePath(k));
    m_out.appendTo(genericPath);
    join.add(Number(AOT_TYPED_OPERATION(Pow, number(x), y)));
    return join.finish().as<Number>();
}

static bool isExactInteger(const Type& type, int64_t limit = maxExactInteger)
{
    return type.isInteger() && type.integers.isSubsetOf({ -limit, limit });
}

Integer Lowering::divide(Integer x, Range dividends, Integer y, Range divisors)
{
    if (dividends.minimum >= 0 && divisors.minimum > 0 && dividends.isSubsetOf(Range::uint32()) && divisors.isSubsetOf(Range::uint32()))
        return zeroExtended(Word32(m_out.m_block->appendNew<B3::Value>(m_proc, UDiv, m_out.origin(), low32(x).value(), low32(y).value())));
    if (divisors.minimum == divisors.maximum && hasOneBitSet(divisors.minimum)) {
        unsigned shift = WTF::fastLog2(static_cast<uint64_t>(divisors.minimum));
        if (!shift)
            return x;
        if (dividends.minimum < 0)
            x = add(x, Integer(m_out.lShr(m_out.aShr(x.value(), m_out.constInt32(63)), m_out.constInt32(64 - shift))));
        return Integer(m_out.aShr(x.value(), m_out.constInt32(shift)));
    }
    if (dividends.isSubsetOf(Range::int32()) && divisors.isSubsetOf(Range::int32()) && (dividends.minimum > INT32_MIN || divisors.minimum > 0))
        return signExtended(Word32(m_out.div(low32(x).value(), low32(y).value())));
    return Integer(m_out.div(x.value(), y.value()));
}

std::optional<Integer> Lowering::roundedQuotient(InlineBuiltin which, ValueID argument, bool yieldsInteger)
{
    ValueID origin = m_program.originOf(m_function, argument);
    auto& quotient = m_function.at(origin);
    if (quotient.opcode != Opcode::Div || !operandsAreAvailable(origin))
        return std::nullopt;
    int64_t limit = which == InlineBuiltin::MathRound ? maxExactInteger >> 2 : maxExactInteger;
    if (!isExactInteger(typeOf(quotient.child(0)), limit) || !isExactInteger(typeOf(quotient.child(1)), limit))
        return std::nullopt;
    Range dividends = typeOf(quotient.child(0)).integers;
    Range divisors = typeOf(quotient.child(1)).integers;
    bool isDividendConstant = dividends.minimum == dividends.maximum;
    bool isDivisorConstant = divisors.minimum == divisors.maximum;
    if (divisors.contains(0) || (which == InlineBuiltin::MathRound && divisors.minimum < 0) || (isDividendConstant && isDivisorConstant))
        return std::nullopt;
    bool isNeverNegative = dividends.minimum >= 0 && divisors.minimum > 0;
    if (!yieldsInteger && !isNeverNegative && !(which == InlineBuiltin::MathFloor && divisors.minimum > 0))
        return std::nullopt;

    Integer x = in<Integer>(quotient.child(0));
    Integer y = in<Integer>(quotient.child(1));
    if (which == InlineBuiltin::MathRound) {
        x = add(isDividendConstant ? integer(2 * dividends.minimum) : add(x, x), y);
        y = isDivisorConstant ? integer(2 * divisors.minimum) : add(y, y);
        dividends = { 2 * dividends.minimum + divisors.minimum, 2 * dividends.maximum + divisors.maximum };
        divisors = { 2 * divisors.minimum, 2 * divisors.maximum };
        which = InlineBuiltin::MathFloor;
    }
    bool roundsUp = which == InlineBuiltin::MathCeil;
    if (isDivisorConstant && hasOneBitSet(divisors.minimum) && which != InlineBuiltin::MathTrunc) {
        if (roundsUp)
            x = add(x, integer(divisors.minimum - 1));
        return Integer(m_out.aShr(x.value(), m_out.constInt32(WTF::fastLog2(static_cast<uint64_t>(divisors.minimum)))));
    }
    if (roundsUp && isNeverNegative) {
        if (isDivisorConstant)
            x = add(x, integer(divisors.minimum - 1));
        else if (isDividendConstant)
            x = add(y, integer(dividends.minimum - 1));
        else
            x = add(x, sub(y, integer(1)));
        dividends.maximum += divisors.maximum - 1;
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

std::optional<Word32> Lowering::quotientToInt32(ValueID id)
{
    auto& quotient = m_function.at(id);
    if (quotient.opcode != Opcode::Div || !operandsAreAvailable(id) || !isExactInteger(typeOf(quotient.child(0))) || !isExactInteger(typeOf(quotient.child(1))))
        return std::nullopt;
    Integer x = in<Integer>(quotient.child(0));
    Integer y = in<Integer>(quotient.child(1));
    Range dividends = typeOf(quotient.child(0)).integers;
    Range divisors = typeOf(quotient.child(1)).integers;
    if (!divisors.contains(0))
        return low32(divide(x, dividends, y, divisors));
    if (dividends.isSubsetOf(Range::int32()) && divisors.isSubsetOf(Range::int32()))
        return Word32(m_out.chillDiv(low32(x).value(), low32(y).value()));
    return low32(Integer(m_out.chillDiv(x.value(), y.value())));
}

std::optional<Operand> Lowering::lowerNumberFromSubstring(ValueID result, const Value& value)
{
    if (value.arguments.isEmpty() || !m_substringsOnlyInspected.get(value.arguments[0].index()))
        return std::nullopt;
    auto& implementation = m_builtins.at(value.builtin()).implementation;
    if (implementation.kind != BuiltinImplementation::Inline || (implementation.inlined != InlineBuiltin::ToNumber && implementation.inlined != InlineBuiltin::ParseInt))
        return std::nullopt;
    const Substring* part = substringCreatedBy(value.arguments[0]);
    if (!part) {
        auto found = m_optionalSubstrings.find(value.arguments[0].index());
        RELEASE_ASSERT(found != m_optionalSubstrings.end());
        part = &found->value;
    }
    bool isParseInt = implementation.inlined == InlineBuiltin::ParseInt;
    Characters characters = charactersOf(part->base);
    Word32 length = low32(lengthOf(*part));
    Word32 radix = isParseInt && value.arguments.size() > 1 ? low32(in<Integer>(value.arguments[1])) : word32(0);
    auto parse = [&] {
        return either<Number>(hasBits(characters.flags, StringImpl::flagIs8Bit()), [&] {
            Pointer data = offsetBy(characters.data, part->from, 0);
            return isParseInt ? AOT_TYPED_OPERATION(ParseInt8, data, length, radix) : AOT_TYPED_OPERATION(StringToNumber8, data, length);
        }, [&] {
            Integer data(offsetBy(characters.data, part->from, 1).value());
            return isParseInt ? AOT_TYPED_OPERATION(ParseInt16, data, length, radix) : AOT_TYPED_OPERATION(StringToNumber16, data, length);
        });
    };
    Number yielded = part->isPresent.value() ? either<Number>(part->isPresent, parse, [&] {
        return number(PNaN);
    }) : parse();
    ensureStillAliveHere(characters.owner);
    return numberResult(yielded, typeOf(result));
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
    auto ofLatin1Characters = [&](ValueID string, auto&& functor) {
        Rep rep = repOf(type);
        LBasicBlock otherwise = newBlockForGenericCall(value);
        Join join(m_out, rep, b3TypeOf(rep));
        Characters characters = charactersOf(stringReceiver(get(string).as<CellPointer>()), otherwise);
        orElse(hasBits(characters.flags, StringImpl::flagIs8Bit()), otherwise);
        Number yielded = functor(characters);
        ensureStillAliveHere(characters.owner);
        join.add(numberResult(yielded, type));
        if (!isSharedGenericPath(otherwise)) {
            m_out.appendTo(otherwise);
            Boxed genericPath = callBuiltinFunction(value, OperationEffects { writes, true });
            join.add(guaranteesResultType(m_program, m_function, m_function.at(result)) ? convert(genericPath, type, rep) : ensure(genericPath, type));
        }
        return join.finish();
    };

    switch (which) {
    case InlineBuiltin::MathFloor:
    case InlineBuiltin::MathCeil:
    case InlineBuiltin::MathTrunc:
    case InlineBuiltin::MathRound:
        if (typeOf(arguments[0]).isInteger())
            return valueResult(arguments[0], type);
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
            return ensure(magnitude, Type::integer({ 0, std::max(-range.minimum, range.maximum) }), type);
        }
        [[fallthrough]];
    case InlineBuiltin::MathFround:
    case InlineBuiltin::MathSqrt: {
        Number x = asNumericOperand(arguments[0]);
        switch (which) {
        case InlineBuiltin::MathAbs:
            return numberResult(abs(x), type);
        case InlineBuiltin::MathFloor:
            return numberResult(floor(x), type);
        case InlineBuiltin::MathCeil:
            return numberResult(ceil(x), type);
        case InlineBuiltin::MathTrunc:
            return numberResult(trunc(x), type);
        case InlineBuiltin::MathRound: {
            Number integer = ceil(x);
            return numberResult(select(greaterThan(sub(integer, number(0.5)), x), sub(integer, number(1)), integer), type);
        }
        case InlineBuiltin::MathFround:
            return numberResult(Number(m_out.fround(x.value())), type);
        default:
            return numberResult(sqrt(x), type);
        }
    }
    case InlineBuiltin::MathSign: {
        if (typeOf(arguments[0]).isInteger()) {
            Integer x = in<Integer>(arguments[0]);
            Integer isNegative(m_out.aShr(x.value(), m_out.constInt32(63)));
            Integer isPositive(m_out.lShr(neg(x).value(), m_out.constInt32(63)));
            return ensure(bitOr(isNegative, isPositive), Type::integer({ -1, 1 }), type);
        }
        Number x = asNumericOperand(arguments[0]);
        return numberResult(select(greaterThan(x, number(0)), number(1), select(lessThan(x, number(0)), number(-1), x)), type);
    }
    case InlineBuiltin::MathFunction: {
        Number x = asNumericOperand(arguments[0]);
#define AOT_TYPED_CALL_MATH_FUNCTION(capitalizedName, lowerName) \
        if (builtin.name == ASCIILiteral::fromLiteralUnsafe(#lowerName)) \
            return numberResult(AOT_TYPED_OPERATION(Math ## capitalizedName, x), type);
        FOR_EACH_ARITH_UNARY_OP(AOT_TYPED_CALL_MATH_FUNCTION)
#undef AOT_TYPED_CALL_MATH_FUNCTION
        RELEASE_ASSERT_NOT_REACHED();
    }
    case InlineBuiltin::MathATan2:
        return numberResult(AOT_TYPED_OPERATION(MathATan2, asNumericOperand(arguments[0]), asNumericOperand(arguments[1])), type);
    case InlineBuiltin::MathHypot:
        switch (arguments.size()) {
        case 0:
            return numberResult(number(0), type);
        case 1:
            return numberResult(abs(asNumericOperand(arguments[0])), type);
        case 2:
            return numberResult(AOT_TYPED_OPERATION(MathHypot, asNumericOperand(arguments[0]), asNumericOperand(arguments[1])), type);
        default:
            return numberResult(AOT_TYPED_OPERATION(MathHypot3, asNumericOperand(arguments[0]), asNumericOperand(arguments[1]), asNumericOperand(arguments[2])), type);
        }
    case InlineBuiltin::MathF16Round:
        return numberResult(AOT_TYPED_OPERATION(MathF16Round, asNumericOperand(arguments[0])), type);
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
        return numberResult(mul(Number(m_out.intToDouble(bits)), number(1.0 / (1ULL << 53))), type);
    }
    case InlineBuiltin::NumberToString: {
        auto& stringType = m_program.at(m_program.types.string);
        auto& receiver = typeOf(value.child(0));
        auto yield = [&](CellPointer string) {
            if (isOnMainPath())
                didPassNullishTest(result, m_currentBlockID);
            return ensure(string, stringType, type);
        };
        auto genericPath = [&] {
            return asCell(callBuiltinFunction(value, OperationEffects { Writes::RuntimeState, true }));
        };
        if (!receiver.isNumber())
            return yield(genericPath());
        if (arguments.isEmpty())
            return yield(toStringCell(value.child(0)));
        Word32 radix = low32(in<Integer>(arguments[0]));
        auto withRadix = [&] {
            if (receiver.isInteger() && receiver.integers.isSubsetOf(Range::int32()))
                return AOT_TYPED_OPERATION(Int32ToStringWithRadix, low32(in<Integer>(value.child(0))), radix);
            return AOT_TYPED_OPERATION(NumberToStringWithRadix, in<Number>(value.child(0)), radix);
        };
        if (typeOf(arguments[0]).integers.isSubsetOf({ 2, 36 }))
            return yield(withRadix());
        return yield(either<CellPointer>(below(binary<Word32>(&Output::sub, radix, word32(2)), word32(35)), withRadix, genericPath));
    }
    case InlineBuiltin::MathPow: {
        Number base = asNumericOperand(arguments[0]);
        if (auto power = powerWithConstantExponent(base, arguments[1]))
            return numberResult(*power, type);
        if (auto power = powerWithConstantBase(arguments[0], arguments[1]))
            return numberResult(*power, type);
        return numberResult(AOT_TYPED_OPERATION(Pow, base, in<Number>(arguments[1])), type);
    }
    case InlineBuiltin::MathMin:
    case InlineBuiltin::MathMax: {
        bool isMax = which == InlineBuiltin::MathMax;
        if (arguments.isEmpty())
            return numberResult(number(isMax ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity()), type);
        if (areIntegers() && (type.isInteger() || repOf(type) == Rep::JSValue)) {
            Integer chosen = in<Integer>(arguments[0]);
            Range range = typeOf(arguments[0]).integers;
            for (ValueID argument : arguments.span().subspan(1)) {
                Integer next = in<Integer>(argument);
                chosen = select(isMax ? greaterThan(chosen, next) : lessThan(chosen, next), chosen, next);
                range = range.unionWith(typeOf(argument).integers);
            }
            return type.isInteger() ? convert(chosen, type, repOf(type)) : ensure(chosen, Type::integer(range), type);
        }
        Number chosen = asNumericOperand(arguments[0]);
        for (ValueID argument : arguments.span().subspan(1))
            chosen = isMax ? max(chosen, asNumericOperand(argument)) : min(chosen, asNumericOperand(argument));
        return numberResult(chosen, type);
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
        bool testsForNaN = which == InlineBuiltin::IsNaN || which == InlineBuiltin::NumberIsNaN;
        if (!converts && !(typeOf(arguments[0]).primitives & Primitive::Number))
            return booleanResult(boolean(false), type);
        Operand operand = get(arguments[0]);
        if (converts && (operand.rep == Rep::OptionalInt64 || operand.rep == Rep::OptionalDouble)) {
            Number x = asNumericOperand(arguments[0]);
            return booleanResult(testsForNaN ? notEqualOrUnordered(x, x) : isFinite(x), type);
        }
        if (which == InlineBuiltin::NumberIsNaN && operand.rep == Rep::OptionalDouble) {
            Number x = assumedNumber(operand.as<OptionalNumber>());
            return booleanResult(logicalAnd(notEqualOrUnordered(x, x), logicalNot(isMarker(operand))), type);
        }
        if (operand.rep == Rep::Int64) {
            if (which == InlineBuiltin::NumberIsSafeInteger)
                return booleanResult(isSubsetOf(operand.as<Integer>(), safeIntegers), type);
            return booleanResult(boolean(!testsForNaN), type);
        }
        if (operand.rep == Rep::OptionalInt64) {
            if (testsForNaN)
                return booleanResult(boolean(false), type);
            if (which == InlineBuiltin::NumberIsSafeInteger)
                return booleanResult(isSubsetOf(assumedInteger(operand.as<OptionalInteger>()), safeIntegers), type);
            return booleanResult(logicalNot(isMarker(operand)), type);
        }
        auto resultFor = [&](Number x) {
            switch (which) {
            case InlineBuiltin::IsNaN:
            case InlineBuiltin::NumberIsNaN:
                return notEqualOrUnordered(x, x);
            case InlineBuiltin::IsFinite:
            case InlineBuiltin::NumberIsFinite:
                return isFinite(x);
            case InlineBuiltin::NumberIsInteger:
                return isFiniteInteger(x);
            default:
                return logicalAnd(equal(trunc(x), x), lessThanOrEqual(abs(x), number(static_cast<double>(safeIntegers.maximum))));
            }
        };
        if (operand.rep == Rep::JSValue) {
            Boxed value = operand.as<Boxed>();
            return booleanResult(either<Word32>(isInt32(value), [&] {
                return boolean(!testsForNaN);
            }, [&] {
                return either<Word32>(isNumber(value), [&] {
                    return resultFor(unboxedDouble(value));
                }, [&] {
                    return boolean(false);
                });
            }), type);
        }
        return booleanResult(resultFor(operand.rep == Rep::Double ? operand.as<Number>() : assumedNumber(operand.as<OptionalNumber>())), type);
    }
    case InlineBuiltin::ToNumber:
        if (arguments.isEmpty())
            return ensure(integer(0), Type::integer(Range::one(0)), type);
        if (typeOf(arguments[0]).isString()) {
            return ofLatin1Characters(arguments[0], [&](const Characters& characters) {
                return AOT_TYPED_OPERATION(StringToNumber8, characters.data, low32(characters.length));
            });
        }
        if (!typeOf(arguments[0]).isNumber()) {
            Rep rep = repOf(type);
            Boxed operand = in<Boxed>(arguments[0]);
            LBasicBlock notCell = m_out.newBlock();
            LBasicBlock otherwise = newBlockForGenericCall(value);
            Join join(m_out, rep, b3TypeOf(rep));
            orElse(isCell(operand), notCell);
            CellPointer cell = asCell(operand);
            orElse(equal(loadCellType(cell), word32(StringType)), otherwise);
            Characters characters = charactersOf(stringReceiver(cell), otherwise);
            orElse(hasBits(characters.flags, StringImpl::flagIs8Bit()), otherwise);
            Number yielded = AOT_TYPED_OPERATION(StringToNumber8, characters.data, low32(characters.length));
            ensureStillAliveHere(characters.owner);
            join.add(numberResult(yielded, type));
            m_out.appendTo(notCell);
            orElse(isNumber(operand), otherwise);
            join.add(ensure(operand, m_program.at(m_program.types.number), type));
            if (!isSharedGenericPath(otherwise)) {
                m_out.appendTo(otherwise);
                Boxed genericPath = callBuiltinFunction(value, OperationEffects { writes, true });
                join.add(guaranteesResultType(m_program, m_function, m_function.at(result)) ? convert(genericPath, type, rep) : ensure(genericPath, type));
            }
            return join.finish();
        }
        return valueResult(arguments[0], type);
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
