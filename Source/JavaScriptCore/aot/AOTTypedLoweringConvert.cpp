/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "B3ReduceStrength.h"
#include "B3StackmapGenerationParams.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT { namespace Typed {

using namespace B3;

Integer Lowering::truncateToInteger(Number value)
{
    static_assert(static_cast<int32_t>(PureFunctionOfChildren::TruncateToInteger) == toInt64TruncationKey && static_cast<int32_t>(PureFunctionOfChildren::ToInt32) == toInt32ConversionKey);
    if (value.value()->hasDouble() && std::abs(value.value()->asDouble()) < static_cast<double>(maxTrackedInteger))
        return integer(static_cast<int64_t>(value.value()->asDouble()));
    PatchpointValue* patchpoint = m_out.patchpoint(Int64);
    patchpoint->append(value.value(), ValueRep::SomeRegister);
    patchpoint->effects = Effects::none();
    patchpoint->pureFunctionOfChildrenKey = static_cast<int32_t>(PureFunctionOfChildren::TruncateToInteger);
    patchpoint->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
        jit.truncateDoubleToInt64(params[1].fpr(), params[0].gpr());
    });
    m_asDouble.add(patchpoint, value.value());
    return Integer(patchpoint);
}

Number Lowering::toNumber(Integer value)
{
    if (LValue original = m_asDouble.get(value.value()))
        return Number(original);
    return convertToNumber(value);
}

Integer Lowering::bitsOf(Operand operand)
{
    if (b3TypeOf(operand.rep) == Double)
        return Integer(m_out.bitCast(operand.value, Int64));
    RELEASE_ASSERT(b3TypeOf(operand.rep) == Int64);
    return Integer(operand.value);
}

Operand Lowering::fromBits(Integer bits, Rep rep)
{
    if (b3TypeOf(rep) == Double)
        return Operand(rep, m_out.bitCast(bits.value(), Double));
    RELEASE_ASSERT(b3TypeOf(rep) == Int64);
    return Operand(rep, bits.value());
}

static Rep plain(Rep rep)
{
    if (rep == Rep::OptionalInt64)
        return Rep::Int64;
    if (rep == Rep::OptionalDouble)
        return Rep::Double;
    return rep;
}

static constexpr uint16_t markerKinds[] = { Primitive::Undefined, Primitive::Null, Primitive::Absent };

static std::array<int64_t, 3> markersIn(Rep rep)
{
    switch (rep) {
    case Rep::OptionalInt64:
        return { undefinedInteger, nullInteger, absentInteger };
    case Rep::OptionalDouble:
        return { static_cast<int64_t>(undefinedDouble), static_cast<int64_t>(nullDouble), static_cast<int64_t>(absentDouble) };
    case Rep::JSValue:
    case Rep::LiteralString:
        return { JSValue::encode(jsUndefined()), JSValue::encode(jsNull()), JSValue::encode(JSValue()) };
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

int64_t Lowering::markerBits(uint16_t kind, Rep rep)
{
    for (unsigned i = 0; i < std::size(markerKinds); ++i) {
        if (markerKinds[i] == kind)
            return markersIn(rep)[i];
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Word32 Lowering::isMarker(Operand operand)
{
    RELEASE_ASSERT(plain(operand.rep) != operand.rep);
    static_assert(absentDouble > undefinedDouble && absentInteger > undefinedInteger);
    return greaterThanOrEqual(bitsOf(beforeAbsentNormalization(operand)), integer(markerBits(Primitive::Undefined, operand.rep)));
}

Word32 Lowering::isAbsent(Operand raw)
{
    if (LValue index = indexOf(raw.value))
        return Word32(m_out.isZero64(index));
    return equal(bitsOf(raw), integer(markerBits(Primitive::Absent, raw.rep)));
}

Number Lowering::purified(Number value)
{
    return select(equal(value, value), value, number(PNaN));
}

Boxed Lowering::boxNumber(Number value)
{
#if CPU(ARM64)
    if (MacroAssembler::supportsDoubleToInt32ConversionUsingJavaScriptSemantics()) {
        if (value.value()->hasDouble())
            return boxedConstant(jsNumber(purifyNaN(value.value()->asDouble())));
        if (value.value()->opcode() == B3::IToD && value.value()->child(0)->type() == Int32)
            return boxInt32(Word32(value.value()->child(0)));
        PatchpointValue* patchpoint = m_out.patchpoint(Int64);
        patchpoint->append(ConstrainedValue(value.value(), ValueRep::SomeRegister));
        patchpoint->numGPScratchRegisters = 1;
        patchpoint->effects = Effects::none();
        patchpoint->pureFunctionOfChildrenKey = static_cast<int32_t>(PureFunctionOfChildren::BoxNumber);
        if (!m_context.numberBoxingGenerator) {
            m_context.numberBoxingGenerator = createSharedTask<StackmapGeneratorFunction>([](CCallHelpers& jit, const StackmapGenerationParams& params) {
                GPRReg result = params[0].gpr();
                FPRReg value = params[1].fpr();
                GPRReg boxedDouble = params.gpScratch(0);
                jit.moveDoubleTo64(value, boxedDouble);
                jit.sub64(GPRInfo::numberTagRegister, boxedDouble);
                jit.move(CCallHelpers::TrustedImm64(JSValue::encode(jsNaN())), result);
                jit.compareOnFlagsDouble(value, value);
                jit.m_assembler.csel<64>(boxedDouble, boxedDouble, result, ARM64Assembler::ConditionEQ);
                jit.convertDoubleToInt32UsingJavaScriptSemantics(value, result);
                jit.addZeroExtend64(GPRInfo::numberTagRegister, result, result);
                jit.m_assembler.csel<64>(result, result, boxedDouble, ARM64Assembler::ConditionEQ);
            });
        }
        patchpoint->setGenerator(m_context.numberBoxingGenerator);
        return Boxed(patchpoint);
    }
#elif CPU(X86_64)
    {
        if (value.value()->hasDouble())
            return boxedConstant(jsNumber(purifyNaN(value.value()->asDouble())));
        if (value.value()->opcode() == B3::IToD && value.value()->child(0)->type() == Int32)
            return boxInt32(Word32(value.value()->child(0)));
        PatchpointValue* patchpoint = m_out.patchpoint(Int64);
        patchpoint->append(ConstrainedValue(value.value(), ValueRep::SomeRegister));
        patchpoint->numGPScratchRegisters = 2;
        patchpoint->numFPScratchRegisters = 1;
        patchpoint->effects = Effects::none();
        patchpoint->pureFunctionOfChildrenKey = static_cast<int32_t>(PureFunctionOfChildren::BoxNumber);
        if (!m_context.numberBoxingGenerator) {
            m_context.numberBoxingGenerator = createSharedTask<StackmapGeneratorFunction>([](CCallHelpers& jit, const StackmapGenerationParams& params) {
                GPRReg result = params[0].gpr();
                FPRReg value = params[1].fpr();
                GPRReg boxedDouble = params.gpScratch(0);
                GPRReg difference = params.gpScratch(1);
                FPRReg converted = params.fpScratch(0);
                jit.moveDoubleTo64(value, boxedDouble);
                jit.truncateDoubleToInt32(value, result);
                jit.convertInt32ToDouble(result, converted);
                jit.moveDoubleTo64(converted, difference);
                jit.add64(CCallHelpers::Address(instanceGPR, Instance::offsetOfNumberTag()), result);
                jit.xor64(boxedDouble, difference);
                jit.sub64(CCallHelpers::Address(instanceGPR, Instance::offsetOfNumberTag()), boxedDouble);
                if (CCallHelpers::supportsAVX())
                    jit.m_assembler.vucomisd_rr(value, value);
                else
                    jit.m_assembler.ucomisd_rr(value, value);
                jit.m_assembler.cmovq_mr(X86Assembler::ConditionP, static_cast<int>(Instance::offsetOfBoxedNaN()), instanceGPR, boxedDouble);
                jit.m_assembler.testq_rr(difference, difference);
                jit.m_assembler.cmovq_rr(X86Assembler::ConditionNE, boxedDouble, result);
            });
        }
        patchpoint->setGenerator(m_context.numberBoxingGenerator);
        return Boxed(patchpoint);
    }
#endif
    Word32 low = low32(truncateToInteger(value));
    return select(equal(bitsOf(toNumber(low)), bitsOf(value)), boxInt32(low), box(purified(value)));
}

bool Lowering::tryTailCallBoxNumber(ValueID returned)
{
    Operand operand = get(returned);
    if ((operand.rep != Rep::Double && operand.rep != Rep::OptionalDouble) || m_boxedBeforeUnboxing.get(operand.value) || operand.value->hasDouble() || (operand.value->opcode() == B3::IToD && operand.value->child(0)->type() == Int32))
        return false;
    PatchpointValue* jump = m_out.patchpoint(Void);
    jump->append(ConstrainedValue(operand.value, ValueRep::reg(FPRInfo::returnValueFPR)));
    jump->clobber(RegisterSet::macroClobberedGPRs());
    jump->effects.terminal = true;
    jump->setGenerator([context = &m_context, stub = operand.rep == Rep::Double ? Stub::BoxNumber : Stub::BoxOptionalNumber](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        context->emitFrameTeardown(jit, params.proc().code());
        context->stubCalls.tailCall(jit, stub);
    });
    return true;
}

Word32 Lowering::isSubsetOf(Integer value, Range range)
{
    if (range.minimum > range.maximum)
        return word32(0);
    if (range.minimum == range.maximum)
        return equal(value, integer(range.minimum));
    if (range == Range::int32())
        return equal(signExtended(low32(value)), value);
    Integer fromMinimum = range.minimum ? Integer(m_out.sub(value.value(), integer(range.minimum).value())) : value;
    return Word32(m_out.belowOrEqual(fromMinimum.value(), integer(static_cast<int64_t>(static_cast<uint64_t>(range.maximum) - static_cast<uint64_t>(range.minimum))).value()));
}

Operand Lowering::convertOptional(Operand operand, const Type& type, Rep to)
{
    Rep from = operand.rep;
    std::optional<Operand> result;
    if (type.primitives & Primitive::Number) {
        Operand converted;
        if (from == Rep::OptionalDouble && to == Rep::JSValue)
            converted = boxNumber(assumedNumber(operand.as<OptionalNumber>()));
        else
            converted = convert(Operand(plain(from), (plain(to) == to && to != Rep::JSValue ? beforeAbsentNormalization(operand) : operand).value), type.withoutPrimitives(static_cast<uint16_t>(~Primitive::Number)), plain(to));
        if (from == Rep::Double && to == Rep::OptionalDouble) {
            Number number = converted.as<Number>();
            converted = either<Number>(equal(number, number), [&] {
                return number;
            }, [&] {
                return this->number(PNaN);
            });
        }
        result = Operand(to, converted.value);
    }
    if (plain(to) == to && to != Rep::JSValue) {
        if (!result)
            return failWith("a conversion of a non-number to a number"_s);
        return *result;
    }
    bool mayBeMarker = plain(from) != from || from == Rep::JSValue;
    Integer bits;
    for (unsigned i = 0; i < std::size(markerKinds); ++i) {
        if (!(type.primitives & markerKinds[i]))
            continue;
        Operand marker = fromBits(integer(markersIn(to)[i]), to);
        if (!result) {
            result = marker;
            continue;
        }
        if (!mayBeMarker)
            continue;
        if (!bits)
            bits = bitsOf(operand);
        result = Operand(to, m_out.select(equal(bits, integer(markersIn(from)[i])).value(), marker.value, result->value));
    }
    if (result && from == Rep::JSValue && to == Rep::OptionalDouble && m_out.m_block->size() && m_out.m_block->last() == result->value)
        m_boxedBeforeUnboxing.add(result->value, operand.value);
    return result ? *result : fromBits(integer(0), to);
}

LiteralStringWord Lowering::literalStringConstant(StringID string)
{
    if (auto number = m_program.literalStringIndex(string))
        return LiteralStringWord(m_out.constInt64(literalStringWord(*number)));
    return LiteralStringWord(stringConstant(string).value());
}

Boxed Lowering::boxLiteralString(LiteralStringWord word)
{
    if (word.value()->hasInt64()) {
        uint64_t bits = word.value()->asInt64();
        if (isLiteralStringWord(bits))
            return box(stringConstant(m_program.literalStrings[literalStringIndex(static_cast<uint32_t>(bits))]));
        return Boxed(word.value());
    }
    if (auto found = m_unboxedLiteralStrings.find(word.value()); found != m_unboxedLiteralStrings.end())
        return Boxed(found->value);
    Boxed boxed(callAllocationStub(Stub::LiteralStringToString, { { word.value(), stubTemporaryGPRs[2] } }, { Writes::Pure, false }, false));
    m_boxedLiteralStrings.add(boxed.value(), word.value());
    return boxed;
}

LiteralStringWord Lowering::literalStringOf(Operand operand, const Type& type)
{
    RELEASE_ASSERT(operand.rep == Rep::Cell || operand.rep == Rep::JSValue);
    if (auto found = m_boxedLiteralStrings.find(operand.value); found != m_boxedLiteralStrings.end())
        return LiteralStringWord(found->value);
    if (!(type.primitives & (Primitive::String | Primitive::LiteralString)))
        return LiteralStringWord(operand.value);
    Boxed boxed = operand.rep == Rep::Cell ? box(operand.as<CellPointer>()) : rematerializedValue(operand.as<Boxed>());
    LiteralStringWord word(callAllocationStub(Stub::ToLiteralString, { { boxed.value(), stubTemporaryGPRs[2] } }, { Writes::Pure, false }, false));
    m_unboxedLiteralStrings.add(word.value(), boxed.value());
    return word;
}

Operand Lowering::convertLiteralString(Operand operand, const Type& type, Rep to)
{
    if (to == Rep::LiteralString) {
        if (operand.rep != Rep::Cell && operand.rep != Rep::JSValue)
            return failWith("a conversion of a non-string to a string"_s);
        return literalStringOf(operand, type);
    }
    Boxed boxed = boxLiteralString(operand.as<LiteralStringWord>());
    if (to == Rep::JSValue)
        return boxed;
    if (to == Rep::Cell)
        return asCell(boxed);
    return failWith("a conversion of a string to a non-string"_s);
}

Operand Lowering::convert(Operand operand, const Type& type, Rep to)
{
    Rep from = operand.rep;
    RELEASE_ASSERT(from != Rep::Void && to != Rep::Void);
    if (from != to && (from == Rep::LiteralString || to == Rep::LiteralString))
        return convertLiteralString(operand, type, to);
    if (from == to)
        return operand;
    if (to == Rep::JSValue) {
        if (LValue boxed = m_boxedBeforeUnboxing.get(operand.value))
            return Boxed(boxed);
    }
    if (plain(from) != from || plain(to) != to)
        return convertOptional(operand, type, to);
    if (from == Rep::Int32 && !(type.primitives & Primitive::Boolean)) {
        bool isNotNegative = type.primitives & Primitive::Int && type.integers.minimum >= 0;
        operand = isNotNegative ? zeroExtended(operand.as<Word32>()) : signExtended(operand.as<Word32>());
        from = Rep::Int64;
        if (to == Rep::Int64)
            return operand;
    }
    switch (from) {
    case Rep::Cell:
        if (to == Rep::JSValue)
            return box(operand.as<CellPointer>());
        break;
    case Rep::Int32:
        if (to == Rep::JSValue)
            return Boxed(boxBoolean(operand.value));
        break;
    case Rep::Int64: {
        Integer value = operand.as<Integer>();
        if (to == Rep::Int32)
            return low32(value);
        if (to == Rep::Double) {
            Number number = toNumber(value);
            if (type.isInteger() && number.value()->opcode() == B3::IToD && number.value()->child(0) == value.value())
                m_lastIntegerConvertedToNumber = { number.value(), value.value(), type.integers.minimum };
            return number;
        }
        if (to == Rep::JSValue) {
            Word32 low = low32(value);
            if (type.primitives & Primitive::Int && type.integers.isSubsetOf(Range::int32()))
                return boxInt32(low);
            bool isNeverNegative = type.primitives & Primitive::Int && type.integers.minimum >= 0;
            Word32 isInt32 = isNeverNegative ? belowOrEqual(value, integer(INT32_MAX)) : equal(signExtended(low), value);
            if (m_isInLoop)
                return select(isInt32, boxInt32(low), box(toNumber(value)));
            return either<Boxed>(isInt32, [&] {
                return boxInt32(low);
            }, [&] {
                return box(toNumber(value));
            });
        }
        break;
    }
    case Rep::Double: {
        Number value = operand.as<Number>();
        if (to == Rep::Int64)
            return truncateToInteger(value);
        if (to == Rep::Int32)
            return low32(truncateToInteger(value));
        if (to == Rep::JSValue)
            return boxNumber(value);
        break;
    }
    case Rep::JSValue: {
        Boxed value = operand.as<Boxed>();
        if (to == Rep::Cell)
            return asCell(value);
        if (to == Rep::Int32 && type.primitives & Primitive::Boolean)
            return unboxedBoolean(value);
        if (to == Rep::Double) {
            Number number = select(isInt32(value), toNumber(low32(value)), unboxedDouble(value));
            if (m_out.m_block->size() && m_out.m_block->last() == number.value())
                m_boxedBeforeUnboxing.add(number.value(), value.value());
            return number;
        }
        Integer unboxed = select(isInt32(value), signExtended(low32(value)), truncateToInteger(unboxedDouble(value)));
        if (to == Rep::Int64)
            return unboxed;
        if (to == Rep::Int32)
            return low32(unboxed);
        break;
    }
    default:
        break;
    }
    return failWith("a conversion between these representations"_s);
}

Word32 Lowering::toInt32(Number value)
{
#if CPU(ARM64)
    if (MacroAssembler::supportsDoubleToInt32ConversionUsingJavaScriptSemantics()) {
        PatchpointValue* patchpoint = m_out.patchpoint(Int32);
        patchpoint->append(ConstrainedValue(value.value(), ValueRep::SomeRegister));
        patchpoint->effects = Effects::none();
        patchpoint->pureFunctionOfChildrenKey = static_cast<int32_t>(PureFunctionOfChildren::ToInt32);
        patchpoint->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
            jit.convertDoubleToInt32UsingJavaScriptSemantics(params[1].fpr(), params[0].gpr());
        });
        return Word32(patchpoint);
    }
#endif
    Integer truncated = truncateToInteger(value);
    Word32 fits = below(integer(1), add(truncated, integer(std::numeric_limits<int64_t>::min() + 1)));
    return either<Word32>(fits, [&] {
        return low32(truncated);
    }, [&] {
        return AOT_TYPED_OPERATION(ToInt32, value);
    });
}

Word32 Lowering::toInt32OrElse(ValueID id, LBasicBlock otherwise)
{
    Operand operand = get(id);
    if (operand.rep != Rep::JSValue)
        return toInt32(id);
    Boxed boxed = operand.as<Boxed>();
    return either<Word32>(isInt32(boxed), [&] {
        return low32(boxed);
    }, [&] {
        orElse(isNumber(boxed), otherwise);
        return toInt32(unboxedDouble(boxed));
    });
}

Word32 Lowering::toInt32(ValueID id)
{
    if (typeOf(id).isUndefinedOrNull())
        return word32(0);
    Operand operand = get(id);
    switch (operand.rep) {
    case Rep::Int64:
        return low32(operand.as<Integer>());
    case Rep::Int32:
        return operand.as<Word32>();
    case Rep::OptionalInt64:
        static_assert(!static_cast<uint32_t>(undefinedInteger) && !static_cast<uint32_t>(nullInteger) && !static_cast<uint32_t>(absentInteger));
        return low32(assumedInteger(operand.as<OptionalInteger>()));
    default:
        if (auto found = m_numberToInt32.find(operand.value); found != m_numberToInt32.end())
            return Word32(found->value);
        static_assert(!static_cast<uint32_t>(INT64_MIN));
        if (auto found = m_numbersAsIntegers.find(id.index()); found != m_numbersAsIntegers.end() && found->value.minimum && found->value.number == operand.value)
            return low32(Integer(found->value.integer));
        if (LValue phi = m_int32Phis.get(id.index()))
            return Word32(phi);
        if (auto ofSum = sumToInt32(id))
            return *ofSum;
        if (auto ofQuotient = quotientToInt32(id))
            return *ofQuotient;
        if (isInlineBuiltin(id, InlineBuiltin::MathTrunc) && m_function.at(id).arguments.size() == 1 && operandsAreAvailable(id) && isNumericOperand(typeOf(m_function.at(id).arguments[0])))
            return toInt32(m_function.at(id).arguments[0]);
        return toInt32(asNumericOperand(id));
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
